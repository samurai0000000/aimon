/*
 * ClaudeUsageStore.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "ClaudeUsageStore.hxx"
#include "PathUtils.hxx"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>

namespace fs = std::filesystem;

namespace aimon {

namespace {

const int64_t kMaxEpoch = 4102444800LL;   // 2100-01-01T00:00:00Z, exclusive

bool lengthOk(const std::string& s, bool allowEmpty) {
    return (allowEmpty || !s.empty()) && s.size() <= ClaudeUsageStore::kMaxFieldLength;
}

bool tokensOk(int64_t v) {
    return v >= 0 && v <= ClaudeUsageStore::kMaxTokensPerField;
}

bool validRow(const ClaudeUsageRow& r) {
    return lengthOk(r.account, false) && lengthOk(r.messageId, false) && lengthOk(r.requestId, false) &&
           lengthOk(r.model, false) && lengthOk(r.sessionId, true) && r.model != "<synthetic>" &&
           r.timestamp > 0 && r.timestamp < kMaxEpoch && tokensOk(r.input) && tokensOk(r.output) &&
           tokensOk(r.cacheRead) && tokensOk(r.cacheWrite5m) && tokensOk(r.cacheWrite1h) &&
           tokensOk(r.webSearch);
}

std::string dayOf(int64_t epoch) {
    time_t t = static_cast<time_t>(epoch);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    return buf;
}

// Saturating checked arithmetic: on overflow the result is INT64_MAX and
// `overflow` is set.
void addChecked(int64_t& acc, int64_t value, bool& overflow) {
    int64_t out = 0;
    if (__builtin_add_overflow(acc, value, &out)) {
        acc = INT64_MAX;
        overflow = true;
    } else {
        acc = out;
    }
}

void mulAddChecked(int64_t& acc, int64_t tokens, int64_t rate, bool& overflow) {
    int64_t product = 0;
    if (__builtin_mul_overflow(tokens, rate, &product)) {
        acc = INT64_MAX;
        overflow = true;
        return;
    }
    addChecked(acc, product, overflow);
}

// Context for the SQLite user functions used while aggregating: they let the
// grouping choose the price version and rate tier of every message.
struct AggContext {
    const std::vector<PriceVersion>* versions = nullptr;
};

void sqlPriceVersion(sqlite3_context* ctx, int, sqlite3_value** argv) {
    const AggContext* c = static_cast<const AggContext*>(sqlite3_user_data(ctx));
    const int64_t ts = sqlite3_value_int64(argv[0]);
    int chosen = 0;
    for (size_t i = 0; i < c->versions->size(); ++i) {
        if ((*c->versions)[i].effectiveFrom <= ts) {
            chosen = static_cast<int>(i);
        }
    }
    sqlite3_result_int(ctx, chosen);
}

// 1 when the model is tiered in the chosen version and the prompt is over the
// threshold, otherwise 0.
void sqlPriceTier(sqlite3_context* ctx, int, sqlite3_value** argv) {
    const AggContext* c = static_cast<const AggContext*>(sqlite3_user_data(ctx));
    const char* modelText = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
    const int64_t prompt = sqlite3_value_int64(argv[1]);
    const int ver = sqlite3_value_int(argv[2]);
    int tier = 0;
    if (modelText && ver >= 0 && static_cast<size_t>(ver) < c->versions->size()) {
        const auto& models = (*c->versions)[static_cast<size_t>(ver)].models;
        const std::string id = PriceCatalog::resolveModelId(models, modelText);
        if (!id.empty()) {
            const ModelPrice& m = models.at(id);
            if (m.upperTier && m.tierThresholdTokens > 0 && prompt > m.tierThresholdTokens) {
                tier = 1;
            }
        }
    }
    sqlite3_result_int(ctx, tier);
}

class AggFunctions {
public:
    AggFunctions(sqlite3* db, AggContext* ctx) : _db(db) {
        _ok = sqlite3_create_function_v2(db, "aimon_ver", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, ctx,
                                         sqlPriceVersion, nullptr, nullptr, nullptr) == SQLITE_OK &&
              sqlite3_create_function_v2(db, "aimon_tier", 3, SQLITE_UTF8 | SQLITE_DETERMINISTIC, ctx,
                                         sqlPriceTier, nullptr, nullptr, nullptr) == SQLITE_OK;
    }
    ~AggFunctions() {
        sqlite3_create_function_v2(_db, "aimon_ver", 1, SQLITE_UTF8, nullptr, nullptr, nullptr, nullptr, nullptr);
        sqlite3_create_function_v2(_db, "aimon_tier", 3, SQLITE_UTF8, nullptr, nullptr, nullptr, nullptr, nullptr);
    }
    AggFunctions(const AggFunctions&) = delete;
    AggFunctions& operator=(const AggFunctions&) = delete;
    bool ok() const { return _ok; }
private:
    sqlite3* _db;
    bool _ok = false;
};

class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &_stmt, nullptr) != SQLITE_OK) {
            _stmt = nullptr;
        }
    }
    ~Stmt() { if (_stmt) sqlite3_finalize(_stmt); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
    sqlite3_stmt* get() const { return _stmt; }
    explicit operator bool() const { return _stmt != nullptr; }
private:
    sqlite3_stmt* _stmt = nullptr;
};

} // namespace

bool claudeCycleBounds(int64_t nowEpoch, int resetDay, ClaudeCycle& out) {
    if (resetDay < 1 || resetDay > 28 || nowEpoch < 0) {
        return false;
    }
    time_t now = static_cast<time_t>(nowEpoch);
    struct tm tmv;
    gmtime_r(&now, &tmv);
    int year = tmv.tm_year + 1900;
    int month = tmv.tm_mon;             // 0..11
    if (tmv.tm_mday < resetDay) {
        if (--month < 0) {
            month = 11;
            --year;
        }
    }
    auto at = [](int y, int m, int d) {
        struct tm t;
        std::memset(&t, 0, sizeof(t));
        t.tm_year = y - 1900;
        t.tm_mon = m;
        t.tm_mday = d;
        return static_cast<int64_t>(timegm(&t));
    };
    out.startEpoch = at(year, month, resetDay);
    int nextMonth = month + 1;
    int nextYear = year;
    if (nextMonth > 11) {
        nextMonth = 0;
        ++nextYear;
    }
    out.endEpoch = at(nextYear, nextMonth, resetDay);
    return true;
}

ClaudeUsageStore::ClaudeUsageStore() = default;

ClaudeUsageStore::~ClaudeUsageStore() {
    close();
}

bool ClaudeUsageStore::execLocked(const char* sql, std::string& error) const {
    char* msg = nullptr;
    int rc = sqlite3_exec(_db, sql, nullptr, nullptr, &msg);
    if (rc != SQLITE_OK) {
        error = msg ? msg : "sqlite error";
        sqlite3_free(msg);
        return false;
    }
    return true;
}

bool ClaudeUsageStore::open(const std::string& dbPath) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_db) {
        sqlite3_close(_db);
        _db = nullptr;
    }
    std::string path = PathUtils::expandHome(dbPath);
    std::string dir = fs::path(path).parent_path().string();
    if (!dir.empty() && !PathUtils::ensureDirectoryExists(dir)) {
        return false;
    }
    if (sqlite3_open(path.c_str(), &_db) != SQLITE_OK) {
        if (_db) {
            sqlite3_close(_db);
        }
        _db = nullptr;
        return false;
    }
    sqlite3_busy_timeout(_db, 5000);
    std::string error;
    const char* setup =
        "PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=NORMAL;"
        "CREATE TABLE IF NOT EXISTS claude_usage_rows ("
        "  account TEXT NOT NULL, message_id TEXT NOT NULL, request_id TEXT NOT NULL,"
        "  session_id TEXT NOT NULL, ts INTEGER NOT NULL, day TEXT NOT NULL,"
        "  model TEXT NOT NULL, speed TEXT NOT NULL,"
        "  input INTEGER NOT NULL, output INTEGER NOT NULL, cache_read INTEGER NOT NULL,"
        "  cache_w5m INTEGER NOT NULL, cache_w1h INTEGER NOT NULL, web_search INTEGER NOT NULL,"
        "  sidechain INTEGER NOT NULL, split_missing INTEGER NOT NULL,"
        "  PRIMARY KEY (account, message_id, request_id));"
        "CREATE INDEX IF NOT EXISTS idx_claude_rows_ts ON claude_usage_rows (account, ts);"
        "CREATE TABLE IF NOT EXISTS claude_file_cursors ("
        "  account TEXT NOT NULL, path TEXT NOT NULL, offset INTEGER NOT NULL,"
        "  size INTEGER NOT NULL, mtime INTEGER NOT NULL, PRIMARY KEY (account, path));"
        "CREATE TABLE IF NOT EXISTS claude_price_versions ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT, fetched_at INTEGER NOT NULL,"
        "  page_sha256 TEXT NOT NULL, catalog_json TEXT NOT NULL);";
    if (!execLocked(setup, error)) {
        sqlite3_close(_db);
        _db = nullptr;
        return false;
    }
    return true;
}

void ClaudeUsageStore::close() {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_db) {
        sqlite3_close(_db);
        _db = nullptr;
    }
}

ClaudeIngestResult ClaudeUsageStore::ingest(const std::vector<ClaudeUsageRow>& rows) {
    ClaudeIngestResult result;
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_db) {
        result.error = "store is not open";
        return result;
    }
    std::string error;
    if (!execLocked("BEGIN IMMEDIATE;", error)) {
        result.error = error;
        return result;
    }
    Stmt select(_db, "SELECT output FROM claude_usage_rows WHERE account=?1 AND message_id=?2 AND request_id=?3;");
    Stmt upsert(_db,
        "INSERT INTO claude_usage_rows (account, message_id, request_id, session_id, ts, day, model, speed,"
        " input, output, cache_read, cache_w5m, cache_w1h, web_search, sidechain, split_missing)"
        " VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16)"
        " ON CONFLICT(account, message_id, request_id) DO UPDATE SET"
        "  session_id=excluded.session_id, ts=excluded.ts, day=excluded.day, model=excluded.model,"
        "  speed=excluded.speed, input=excluded.input, output=excluded.output,"
        "  cache_read=excluded.cache_read, cache_w5m=excluded.cache_w5m, cache_w1h=excluded.cache_w1h,"
        "  web_search=excluded.web_search, sidechain=excluded.sidechain, split_missing=excluded.split_missing"
        " WHERE excluded.output > claude_usage_rows.output;");
    if (!select || !upsert) {
        execLocked("ROLLBACK;", error);
        result.error = "cannot prepare statements";
        return result;
    }

    for (const ClaudeUsageRow& r : rows) {
        if (!validRow(r)) {
            ++result.rejected;
            continue;
        }
        sqlite3_reset(select.get());
        sqlite3_bind_text(select.get(), 1, r.account.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(select.get(), 2, r.messageId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(select.get(), 3, r.requestId.c_str(), -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(select.get());
        bool exists = (rc == SQLITE_ROW);
        int64_t storedOutput = exists ? sqlite3_column_int64(select.get(), 0) : 0;
        if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
            execLocked("ROLLBACK;", error);
            result.error = sqlite3_errmsg(_db);
            return ClaudeIngestResult{false, result.error, 0, 0, 0, 0};
        }
        if (exists && r.output <= storedOutput) {
            ++result.ignored;
            continue;
        }

        const std::string day = dayOf(r.timestamp);
        const std::string speed = (r.speed == "fast") ? "fast" : "standard";
        sqlite3_reset(upsert.get());
        sqlite3_bind_text(upsert.get(), 1, r.account.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(upsert.get(), 2, r.messageId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(upsert.get(), 3, r.requestId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(upsert.get(), 4, r.sessionId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(upsert.get(), 5, r.timestamp);
        sqlite3_bind_text(upsert.get(), 6, day.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(upsert.get(), 7, r.model.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(upsert.get(), 8, speed.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(upsert.get(), 9, r.input);
        sqlite3_bind_int64(upsert.get(), 10, r.output);
        sqlite3_bind_int64(upsert.get(), 11, r.cacheRead);
        sqlite3_bind_int64(upsert.get(), 12, r.cacheWrite5m);
        sqlite3_bind_int64(upsert.get(), 13, r.cacheWrite1h);
        sqlite3_bind_int64(upsert.get(), 14, r.webSearch);
        sqlite3_bind_int(upsert.get(), 15, r.sidechain ? 1 : 0);
        sqlite3_bind_int(upsert.get(), 16, r.creationSplitMissing ? 1 : 0);
        rc = sqlite3_step(upsert.get());
        if (rc != SQLITE_DONE) {
            std::string msg = sqlite3_errmsg(_db);
            execLocked("ROLLBACK;", error);
            ClaudeIngestResult failed;
            failed.error = msg;
            return failed;
        }
        if (exists) {
            ++result.updated;
        } else {
            ++result.inserted;
        }
    }

    if (!execLocked("COMMIT;", error)) {
        execLocked("ROLLBACK;", error);
        ClaudeIngestResult failed;
        failed.error = "commit failed: " + error;
        return failed;
    }
    result.ok = true;
    return result;
}

bool ClaudeUsageStore::aggregate(const std::string& account, int64_t fromEpoch, int64_t toEpoch,
                                 const std::map<std::string, ModelPrice>& catalog,
                                 std::vector<ClaudeUsageBucket>& out, std::string& error) const {
    PriceVersion only;
    only.models = catalog;
    return aggregate(account, fromEpoch, toEpoch, std::vector<PriceVersion>{only}, out, error);
}

bool ClaudeUsageStore::aggregate(const std::string& account, int64_t fromEpoch, int64_t toEpoch,
                                 const std::vector<PriceVersion>& versionsIn,
                                 std::vector<ClaudeUsageBucket>& out, std::string& error) const {
    out.clear();
    // Versions are ordered by effective time; with none, everything is unpriced.
    std::vector<PriceVersion> versions = versionsIn;
    if (versions.empty()) {
        versions.push_back(PriceVersion());
    }
    std::stable_sort(versions.begin(), versions.end(),
                     [](const PriceVersion& a, const PriceVersion& b) { return a.effectiveFrom < b.effectiveFrom; });

    std::lock_guard<std::mutex> lock(_mutex);
    if (!_db) {
        error = "store is not open";
        return false;
    }
    AggContext ctx;
    ctx.versions = &versions;
    AggFunctions functions(_db, &ctx);   // declared before the statement: released after it is finalized
    if (!functions.ok()) {
        error = "cannot register pricing functions";
        return false;
    }
    Stmt stmt(_db,
        "SELECT day, model, speed, aimon_ver(ts) AS ver,"
        " aimon_tier(model, input + cache_read + cache_w5m + cache_w1h, aimon_ver(ts)) AS tier,"
        " COUNT(*), SUM(input), SUM(output), SUM(cache_read), SUM(cache_w5m), SUM(cache_w1h), SUM(web_search)"
        " FROM claude_usage_rows WHERE account=?1 AND ts>=?2 AND ts<?3"
        " GROUP BY day, model, speed, ver, tier ORDER BY day, model, speed, ver, tier;");
    if (!stmt) {
        error = sqlite3_errmsg(_db);
        return false;
    }
    sqlite3_bind_text(stmt.get(), 1, account.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt.get(), 2, fromEpoch);
    sqlite3_bind_int64(stmt.get(), 3, toEpoch);

    int rc = 0;
    while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
        const std::string day = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        const std::string model = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
        const std::string speed = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
        const int ver = sqlite3_column_int(stmt.get(), 3);
        const bool upper = sqlite3_column_int(stmt.get(), 4) == 1;
        const int64_t count = sqlite3_column_int64(stmt.get(), 5);
        const int64_t input = sqlite3_column_int64(stmt.get(), 6);
        const int64_t output = sqlite3_column_int64(stmt.get(), 7);
        const int64_t cacheRead = sqlite3_column_int64(stmt.get(), 8);
        const int64_t w5m = sqlite3_column_int64(stmt.get(), 9);
        const int64_t w1h = sqlite3_column_int64(stmt.get(), 10);
        const int64_t web = sqlite3_column_int64(stmt.get(), 11);

        if (out.empty() || out.back().day != day || out.back().model != model) {
            ClaudeUsageBucket b;
            b.day = day;
            b.model = model;
            out.push_back(b);
        }
        ClaudeUsageBucket& b = out.back();
        b.messages += count;
        b.input += input;
        b.output += output;
        b.cacheRead += cacheRead;
        b.cacheWrite5m += w5m;
        b.cacheWrite1h += w1h;
        b.webSearch += web;

        const std::map<std::string, ModelPrice>& models = versions[static_cast<size_t>(ver)].models;
        const std::string id = PriceCatalog::resolveModelId(models, model);
        const bool fast = (speed == "fast");
        if (id.empty() || (fast && (upper || !models.at(id).hasFast))) {
            b.unpricedMessages += count;
            continue;
        }
        const ModelPrice& base = models.at(id);
        const ModelPrice& p = (upper && base.upperTier) ? *base.upperTier : base;
        int64_t cost = 0;
        bool overflow = false;
        if (fast) {
            mulAddChecked(cost, input, p.fastInputNano, overflow);
            mulAddChecked(cost, w5m, p.fastWrite5mNano, overflow);
            mulAddChecked(cost, w1h, p.fastWrite1hNano, overflow);
            mulAddChecked(cost, cacheRead, p.fastReadNano, overflow);
            mulAddChecked(cost, output, p.fastOutputNano, overflow);
        } else {
            mulAddChecked(cost, input, p.inputNano, overflow);
            mulAddChecked(cost, w5m, p.write5mNano, overflow);
            mulAddChecked(cost, w1h, p.write1hNano, overflow);
            mulAddChecked(cost, cacheRead, p.readNano, overflow);
            mulAddChecked(cost, output, p.outputNano, overflow);
        }
        addChecked(b.costNano, cost, overflow);
        b.costOverflow = b.costOverflow || overflow;
    }
    if (rc != SQLITE_DONE) {
        error = sqlite3_errmsg(_db);
        out.clear();
        return false;
    }
    return true;
}

VersionSave ClaudeUsageStore::saveCatalogVersion(int64_t fetchedAt, const std::string& pageSha256,
                                                 const std::map<std::string, ModelPrice>& models) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_db || fetchedAt <= 0 || pageSha256.empty() || pageSha256.size() > 128) {
        return VersionSave::Failed;
    }
    {
        Stmt latest(_db, "SELECT page_sha256 FROM claude_price_versions ORDER BY fetched_at DESC, id DESC LIMIT 1;");
        if (!latest) {
            return VersionSave::Failed;
        }
        int rc = sqlite3_step(latest.get());
        if (rc == SQLITE_ROW && pageSha256 == reinterpret_cast<const char*>(sqlite3_column_text(latest.get(), 0))) {
            return VersionSave::Unchanged;
        }
        if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
            return VersionSave::Failed;
        }
    }
    const std::string json = PriceCatalog::modelsToJson(models);
    Stmt insert(_db, "INSERT INTO claude_price_versions (fetched_at, page_sha256, catalog_json) VALUES (?1,?2,?3);");
    if (!insert) {
        return VersionSave::Failed;
    }
    sqlite3_bind_int64(insert.get(), 1, fetchedAt);
    sqlite3_bind_text(insert.get(), 2, pageSha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert.get(), 3, json.c_str(), -1, SQLITE_TRANSIENT);
    return sqlite3_step(insert.get()) == SQLITE_DONE ? VersionSave::Inserted : VersionSave::Failed;
}

bool ClaudeUsageStore::loadCatalogVersions(std::vector<PriceVersion>& out, std::string& error) const {
    out.clear();
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_db) {
        error = "store is not open";
        return false;
    }
    Stmt stmt(_db, "SELECT fetched_at, page_sha256, catalog_json FROM claude_price_versions"
                   " ORDER BY fetched_at, id;");
    if (!stmt) {
        error = sqlite3_errmsg(_db);
        return false;
    }
    int rc = 0;
    while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
        PriceVersion v;
        v.effectiveFrom = sqlite3_column_int64(stmt.get(), 0);
        v.pageSha256 = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
        std::string modelsError;
        if (!PriceCatalog::modelsFromJson(reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2)),
                                          v.models, modelsError)) {
            error = "stored price version at " + std::to_string(v.effectiveFrom) + " is invalid: " + modelsError;
            out.clear();
            return false;
        }
        out.push_back(std::move(v));
    }
    if (rc != SQLITE_DONE) {
        error = sqlite3_errmsg(_db);
        out.clear();
        return false;
    }
    return true;
}

ClaudeUsageTotals ClaudeUsageStore::sum(const std::vector<ClaudeUsageBucket>& buckets) {
    ClaudeUsageTotals t;
    for (const ClaudeUsageBucket& b : buckets) {
        t.messages += b.messages;
        t.input += b.input;
        t.output += b.output;
        t.cacheRead += b.cacheRead;
        t.cacheWrite5m += b.cacheWrite5m;
        t.cacheWrite1h += b.cacheWrite1h;
        t.webSearch += b.webSearch;
        t.unpricedMessages += b.unpricedMessages;
        addChecked(t.costNano, b.costNano, t.costOverflow);
        t.costOverflow = t.costOverflow || b.costOverflow;
    }
    return t;
}

int64_t ClaudeUsageStore::pruneOlderThan(int64_t cutoffEpoch) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_db) {
        return -1;
    }
    Stmt stmt(_db, "DELETE FROM claude_usage_rows WHERE ts < ?1;");
    if (!stmt) {
        return -1;
    }
    sqlite3_bind_int64(stmt.get(), 1, cutoffEpoch);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        return -1;
    }
    return sqlite3_changes(_db);
}

int64_t ClaudeUsageStore::rowCount(const std::string& account) const {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_db) {
        return 0;
    }
    Stmt stmt(_db, "SELECT COUNT(*) FROM claude_usage_rows WHERE account=?1;");
    if (!stmt) {
        return 0;
    }
    sqlite3_bind_text(stmt.get(), 1, account.c_str(), -1, SQLITE_TRANSIENT);
    return sqlite3_step(stmt.get()) == SQLITE_ROW ? sqlite3_column_int64(stmt.get(), 0) : 0;
}

bool ClaudeUsageStore::saveCursor(const ClaudeFileCursor& c) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_db || c.account.empty() || c.path.empty() || c.account.size() > kMaxFieldLength ||
        c.path.size() > 4096 || c.offset < 0 || c.size < 0) {
        return false;
    }
    Stmt stmt(_db, "INSERT OR REPLACE INTO claude_file_cursors (account, path, offset, size, mtime)"
                   " VALUES (?1,?2,?3,?4,?5);");
    if (!stmt) {
        return false;
    }
    sqlite3_bind_text(stmt.get(), 1, c.account.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, c.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt.get(), 3, c.offset);
    sqlite3_bind_int64(stmt.get(), 4, c.size);
    sqlite3_bind_int64(stmt.get(), 5, c.mtime);
    return sqlite3_step(stmt.get()) == SQLITE_DONE;
}

std::vector<ClaudeFileCursor> ClaudeUsageStore::loadCursors(const std::string& account) const {
    std::vector<ClaudeFileCursor> out;
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_db) {
        return out;
    }
    Stmt stmt(_db, "SELECT account, path, offset, size, mtime FROM claude_file_cursors"
                   " WHERE account=?1 ORDER BY path;");
    if (!stmt) {
        return out;
    }
    sqlite3_bind_text(stmt.get(), 1, account.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        ClaudeFileCursor c;
        c.account = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        c.path = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
        c.offset = sqlite3_column_int64(stmt.get(), 2);
        c.size = sqlite3_column_int64(stmt.get(), 3);
        c.mtime = sqlite3_column_int64(stmt.get(), 4);
        out.push_back(c);
    }
    return out;
}

} // namespace aimon

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
