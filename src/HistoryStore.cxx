/*
 * HistoryStore.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "HistoryStore.hxx"
#include "PathUtils.hxx"
#include <iostream>
#include <filesystem>
#include <chrono>
#include <ctime>

namespace fs = std::filesystem;

namespace aimon {

HistoryStore::HistoryStore() {
}

HistoryStore::~HistoryStore() {
    close();
}

bool HistoryStore::open(const std::string& dbPath) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_db) {
        sqlite3_close(_db);
        _db = nullptr;
    }

    _dbPath = PathUtils::expandHome(dbPath);
    std::string dir = fs::path(_dbPath).parent_path().string();
    if (!dir.empty()) {
        PathUtils::ensureDirectoryExists(dir);
    }

    int rc = sqlite3_open(_dbPath.c_str(), &_db);
    if (rc != SQLITE_OK) {
        std::cerr << "[HistoryStore] Error opening SQLite database " << _dbPath
                  << ": " << sqlite3_errmsg(_db) << std::endl;
        _db = nullptr;
        return false;
    }

    char* errMsg = nullptr;
    sqlite3_exec(_db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);

    const char* createSql =
        "CREATE TABLE IF NOT EXISTS quota_samples ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  timestamp INTEGER NOT NULL,"
        "  provider TEXT NOT NULL,"
        "  metric_key TEXT NOT NULL,"
        "  metric_value REAL NOT NULL,"
        "  metric_limit REAL NOT NULL,"
        "  delta REAL NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_quota_samples "
        "ON quota_samples (provider, metric_key, timestamp);";

    rc = sqlite3_exec(_db, createSql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::cerr << "[HistoryStore] Error creating schema: " << (errMsg ? errMsg : "unknown") << std::endl;
        sqlite3_free(errMsg);
        return false;
    }

    return true;
}

void HistoryStore::close() {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_db) {
        sqlite3_close(_db);
        _db = nullptr;
    }
}

bool HistoryStore::insertSample(time_t timestamp, const std::string& provider,
                                const std::string& metricKey, double value,
                                double limit, double delta) {
    if (!_db) {
        return false;
    }

    const char* insertSql =
        "INSERT INTO quota_samples (timestamp, provider, metric_key, metric_value, metric_limit, delta) "
        "VALUES (?, ?, ?, ?, ?, ?);";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(_db, insertSql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(stmt, 1, timestamp);
    sqlite3_bind_text(stmt, 2, provider.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, metricKey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 4, value);
    sqlite3_bind_double(stmt, 5, limit);
    sqlite3_bind_double(stmt, 6, delta);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return (rc == SQLITE_DONE);
}

bool HistoryStore::recordSnapshot(const AggregateStatus& status) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_db) {
        return false;
    }

    time_t now = std::chrono::system_clock::to_time_t(
        status.lastUpdated.time_since_epoch().count() > 0 ?
        status.lastUpdated : std::chrono::system_clock::now());

    bool isCheckpoint = (_lastCheckpoint == 0 || (now - _lastCheckpoint) >= 3600);
    if (isCheckpoint) {
        _lastCheckpoint = now;
    }

    auto recordMetric = [this, now, isCheckpoint](const std::string& provider,
                                                  const std::string& key,
                                                  double value,
                                                  double limit) {
        std::string fullKey = provider + "." + key;
        auto it = _lastValues.find(fullKey);
        if (it == _lastValues.end()) {
            _lastValues[fullKey] = value;
            insertSample(now, provider, key, value, limit, 0.0);
        } else {
            double delta = value - it->second;
            if (delta != 0.0 || isCheckpoint) {
                insertSample(now, provider, key, value, limit, delta);
                _lastValues[fullKey] = value;
            }
        }
    };

    if (status.antigravity.isRunning) {
        for (const auto& m : status.antigravity.models) {
            recordMetric("antigravity", m.modelId, m.remainingFraction, 1.0);
        }
    }

    if (status.cursor.isAuthenticated) {
        recordMetric("cursor", "fast_requests_used", status.cursor.fastRequestsUsed, status.cursor.fastRequestsLimit);
    }

    // Claude Code: estimated cost and tokens over the configured window, one provider per account.
    for (const auto& acct : status.claude.accounts) {
        if (!acct.hasData || acct.window.costOverflow) {
            continue;
        }
        const std::string provider = "claude:" + acct.name;
        recordMetric(provider, "est_cost_usd", claudeNanoToUsd(acct.window.costNano), acct.spendLimitUsd);
        recordMetric(provider, "total_tokens", static_cast<double>(acct.window.totalTokens()), 0.0);
    }

    return true;
}

std::vector<HistoryRecord> HistoryStore::queryRecentRecords(int limit) {
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<HistoryRecord> records;
    if (!_db) {
        return records;
    }

    const char* querySql =
        "SELECT id, timestamp, provider, metric_key, metric_value, metric_limit, delta "
        "FROM quota_samples ORDER BY timestamp DESC LIMIT ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(_db, querySql, -1, &stmt, nullptr) != SQLITE_OK) {
        return records;
    }

    sqlite3_bind_int(stmt, 1, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        HistoryRecord r;
        r.id = sqlite3_column_int64(stmt, 0);
        r.timestamp = sqlite3_column_int64(stmt, 1);
        r.provider = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        r.metricKey = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        r.metricValue = sqlite3_column_double(stmt, 4);
        r.metricLimit = sqlite3_column_double(stmt, 5);
        r.delta = sqlite3_column_double(stmt, 6);
        records.push_back(r);
    }

    sqlite3_finalize(stmt);
    return records;
}

double HistoryStore::queryTotalUsage(const std::string& provider, const std::string& metricKey, time_t sinceEpoch) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_db) {
        return 0.0;
    }

    const char* sql =
        "SELECT SUM(ABS(delta)) FROM quota_samples "
        "WHERE provider = ? AND metric_key = ? AND timestamp >= ? AND delta < 0;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0.0;
    }

    sqlite3_bind_text(stmt, 1, provider.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, metricKey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, sinceEpoch);

    double total = 0.0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        total = sqlite3_column_double(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return total;
}

std::vector<DailySpendPoint> HistoryStore::queryDailyUsageSummary(const std::string& provider,
                                                                 const std::string& metricKey,
                                                                 time_t startTimestamp,
                                                                 time_t endTimestamp) {
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<DailySpendPoint> points;
    if (!_db) {
        return points;
    }

    const char* sql =
        "SELECT "
        "  strftime('%Y-%m-%d', datetime(timestamp, 'unixepoch')) AS day_str, "
        "  MIN(timestamp) AS min_ts, "
        "  MAX(metric_value) AS max_val "
        "FROM quota_samples "
        "WHERE provider = ? AND metric_key = ? AND timestamp >= ? AND timestamp <= ? "
        "GROUP BY day_str "
        "ORDER BY day_str ASC;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return points;
    }

    sqlite3_bind_text(stmt, 1, provider.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, metricKey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, startTimestamp);
    sqlite3_bind_int64(stmt, 4, endTimestamp);

    int prevCumulative = 0;
    bool isFirst = true;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* dayText = sqlite3_column_text(stmt, 0);
        if (!dayText) continue;

        DailySpendPoint pt;
        pt.dayStr = reinterpret_cast<const char*>(dayText);

        std::tm tm = {};
        if (strptime(pt.dayStr.c_str(), "%Y-%m-%d", &tm)) {
            time_t dayEpoch = timegm(&tm);
            pt.dayMs = static_cast<int64_t>(dayEpoch) * 1000;
        } else {
            time_t minTs = sqlite3_column_int64(stmt, 1);
            pt.dayMs = static_cast<int64_t>(minTs) * 1000;
        }

        double maxVal = sqlite3_column_double(stmt, 2);
        int cumReq = static_cast<int>(maxVal);

        if (isFirst) {
            pt.requestsUsed = cumReq;
            pt.cumulativeRequests = cumReq;
            prevCumulative = cumReq;
            isFirst = false;
        } else {
            pt.requestsUsed = (cumReq >= prevCumulative) ? (cumReq - prevCumulative) : cumReq;
            pt.cumulativeRequests = cumReq;
            prevCumulative = cumReq;
        }

        points.push_back(pt);
    }

    sqlite3_finalize(stmt);
    return points;
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
