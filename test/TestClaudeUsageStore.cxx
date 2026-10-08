/*
 * TestClaudeUsageStore.cxx
 *
 * Qualification tests for ClaudeUsageStore: deduplication, validation,
 * day bucketing, exact integer nano-dollar costing, billing cycle bounds,
 * persistence, retention and concurrent access.
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <thread>
#include <atomic>
#include <map>
#include <string>
#include <vector>
#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ClaudeUsageStore.hxx"
#include "PriceCatalog.hxx"

// CppUTest's memory leak macros redefine `new`; its headers must come last.
#include <CppUTest/CommandLineTestRunner.h>
#include <CppUTest/TestHarness.h>

using namespace aimon;

#ifndef AIMON_SOURCE_DIR
#error "AIMON_SOURCE_DIR must be defined by the build"
#endif

static int64_t epoch(int y, int mo, int d, int h = 0, int mi = 0, int s = 0) {
    struct tm tmv;
    std::memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = y - 1900;
    tmv.tm_mon = mo - 1;
    tmv.tm_mday = d;
    tmv.tm_hour = h;
    tmv.tm_min = mi;
    tmv.tm_sec = s;
    return static_cast<int64_t>(timegm(&tmv));
}

static std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

static std::map<std::string, ModelPrice> realCatalog() {
    PriceParseResult r = PriceCatalog::parsePage(
        readFile(std::string(AIMON_SOURCE_DIR) + "/test/fixtures/pricing_page.md"));
    CHECK_TRUE(r.ok);
    return r.models;
}

static std::map<std::string, ModelPrice> catalogV2() {
    PriceParseResult r = PriceCatalog::parsePage(
        readFile(std::string(AIMON_SOURCE_DIR) + "/test/fixtures/pricing_page_v2.md"));
    CHECK_TRUE(r.ok);
    return r.models;
}

static ClaudeUsageRow makeRow(const std::string& msg, const std::string& req, int64_t ts,
                              const std::string& model = "claude-opus-5-5") {
    ClaudeUsageRow r;
    r.account = "acct";
    r.sessionId = "sess";
    r.messageId = msg;
    r.requestId = req;
    r.timestamp = ts;
    r.model = model;
    return r;
}

static std::string makeTempDir() {
    char tmpl[] = "/tmp/aimon_usage_test_XXXXXX";
    char* dir = mkdtemp(tmpl);
    return dir ? std::string(dir) : std::string();
}

TEST_GROUP(ClaudeUsageStore) {
    std::string dir;
    std::string dbPath;
    ClaudeUsageStore* store = nullptr;
    std::map<std::string, ModelPrice> catalog;

    void setup() {
        dir = makeTempDir();
        CHECK_TRUE(!dir.empty());
        dbPath = dir + "/history.db";
        store = new ClaudeUsageStore();
        CHECK_TRUE(store->open(dbPath));
        catalog = realCatalog();
    }
    void teardown() {
        delete store;
        std::string cmd = "rm -rf '" + dir + "'";
        int rc = std::system(cmd.c_str());
        (void)rc;
    }
    std::vector<ClaudeUsageBucket> all(const std::string& account = "acct") {
        std::vector<ClaudeUsageBucket> out;
        std::string err;
        CHECK_TRUE(store->aggregate(account, 0, 4102444800LL, catalog, out, err));
        return out;
    }
};

TEST(ClaudeUsageStore, InsertsAndAggregatesOneRow) {
    ClaudeUsageRow r = makeRow("m1", "r1", epoch(2026, 10, 8, 10));
    r.input = 10; r.output = 20; r.cacheRead = 5; r.cacheWrite5m = 7;
    ClaudeIngestResult res = store->ingest({r});
    CHECK_TRUE(res.ok);
    LONGS_EQUAL(1, res.inserted);
    LONGS_EQUAL(0, res.updated + res.ignored + res.rejected);

    auto buckets = all();
    LONGS_EQUAL(1, static_cast<long>(buckets.size()));
    STRCMP_EQUAL("2026-10-08", buckets[0].day.c_str());
    STRCMP_EQUAL("claude-opus-5-5", buckets[0].model.c_str());
    LONGS_EQUAL(1, static_cast<long>(buckets[0].messages));
    LONGS_EQUAL(10, static_cast<long>(buckets[0].input));
    LONGS_EQUAL(20, static_cast<long>(buckets[0].output));
}

TEST(ClaudeUsageStore, DuplicateKeyKeepsStrictlyGreatestOutput) {
    ClaudeUsageRow a = makeRow("m1", "r1", epoch(2026, 10, 7, 23, 59, 58));
    a.input = 100; a.output = 10;
    ClaudeUsageRow b = a;
    b.timestamp = epoch(2026, 10, 8, 0, 0, 2);   // later chunk crosses midnight
    b.output = 50;
    ClaudeUsageRow tie = a;
    tie.timestamp = epoch(2026, 10, 8, 0, 0, 3);
    tie.output = 50;
    tie.input = 999;

    CHECK_TRUE(store->ingest({a}).inserted == 1);
    ClaudeIngestResult up = store->ingest({b});
    LONGS_EQUAL(1, up.updated);
    ClaudeIngestResult ties = store->ingest({tie});
    LONGS_EQUAL(1, ties.ignored);
    ClaudeIngestResult lower = store->ingest({a});
    LONGS_EQUAL(1, lower.ignored);

    auto buckets = all();
    LONGS_EQUAL(1, static_cast<long>(buckets.size()));
    STRCMP_EQUAL("2026-10-08", buckets[0].day.c_str());     // day follows the winning row
    LONGS_EQUAL(50, static_cast<long>(buckets[0].output));
    LONGS_EQUAL(100, static_cast<long>(buckets[0].input));  // tie did not replace fields
    LONGS_EQUAL(1, static_cast<long>(buckets[0].messages));
}

TEST(ClaudeUsageStore, ReingestingTheSameBatchIsANoOp) {
    std::vector<ClaudeUsageRow> batch;
    for (int i = 0; i < 20; ++i) {
        ClaudeUsageRow r = makeRow("m" + std::to_string(i), "r" + std::to_string(i), epoch(2026, 10, 8, 1, i));
        r.input = i; r.output = i + 1;
        batch.push_back(r);
    }
    ClaudeIngestResult first = store->ingest(batch);
    LONGS_EQUAL(20, first.inserted);
    auto before = all();
    ClaudeIngestResult second = store->ingest(batch);
    LONGS_EQUAL(20, second.ignored);
    LONGS_EQUAL(0, second.inserted + second.updated);
    auto after = all();
    LONGS_EQUAL(static_cast<long>(before[0].input), static_cast<long>(after[0].input));
    LONGS_EQUAL(20, static_cast<long>(store->rowCount("acct")));
}

TEST(ClaudeUsageStore, ValidationRejectsBadRows) {
    std::vector<ClaudeUsageRow> bad;
    ClaudeUsageRow r = makeRow("m", "r", epoch(2026, 10, 8));
    ClaudeUsageRow x;
    x = r; x.messageId = "";                 bad.push_back(x);
    x = r; x.requestId = "";                 bad.push_back(x);
    x = r; x.account = "";                   bad.push_back(x);
    x = r; x.model = "";                     bad.push_back(x);
    x = r; x.model = "<synthetic>";          bad.push_back(x);
    x = r; x.messageId = std::string(257, 'a'); bad.push_back(x);
    x = r; x.timestamp = 0;                  bad.push_back(x);
    x = r; x.timestamp = -5;                 bad.push_back(x);
    x = r; x.timestamp = 4102444800LL;       bad.push_back(x);
    x = r; x.input = -1;                     bad.push_back(x);
    x = r; x.output = ClaudeUsageStore::kMaxTokensPerField + 1; bad.push_back(x);
    x = r; x.cacheRead = -7;                 bad.push_back(x);
    x = r; x.cacheWrite1h = ClaudeUsageStore::kMaxTokensPerField + 1; bad.push_back(x);
    ClaudeIngestResult res = store->ingest(bad);
    CHECK_TRUE(res.ok);
    LONGS_EQUAL(static_cast<long>(bad.size()), res.rejected);
    LONGS_EQUAL(0, res.inserted);
    LONGS_EQUAL(0, static_cast<long>(store->rowCount("acct")));

    // the maximum allowed value is accepted
    x = r; x.output = ClaudeUsageStore::kMaxTokensPerField;
    LONGS_EQUAL(1, store->ingest({x}).inserted);
}

TEST(ClaudeUsageStore, SpeedIsNormalizedToFastOrStandard) {
    ClaudeUsageRow r = makeRow("m1", "r1", epoch(2026, 10, 8));
    r.speed = "warp"; r.input = 1000;
    CHECK_TRUE(store->ingest({r}).ok);
    auto buckets = all();
    // priced at standard rate: 1000 tokens * 4000 nano
    LONGS_EQUAL(4000000, static_cast<long>(buckets[0].costNano));
}

TEST(ClaudeUsageStore, UtcMidnightSplitsDays) {
    CHECK_TRUE(store->ingest({makeRow("a", "a", epoch(2026, 10, 7, 23, 59, 59)),
                              makeRow("b", "b", epoch(2026, 10, 8, 0, 0, 0))}).ok);
    auto buckets = all();
    LONGS_EQUAL(2, static_cast<long>(buckets.size()));
    STRCMP_EQUAL("2026-10-07", buckets[0].day.c_str());
    STRCMP_EQUAL("2026-10-08", buckets[1].day.c_str());
}

TEST(ClaudeUsageStore, HandComputedCostWithFiveMinuteAndOneHourCacheWrites) {
    ClaudeUsageRow r = makeRow("m1", "r1", epoch(2026, 10, 8, 12));
    r.input = 1000; r.output = 2000; r.cacheRead = 300000; r.cacheWrite5m = 4000; r.cacheWrite1h = 10000;
    CHECK_TRUE(store->ingest({r}).ok);
    // 1000*4000 + 4000*5000 + 10000*8000 + 300000*200 + 2000*20000 = 204,000,000 nano-dollars
    auto buckets = all();
    LONGS_EQUAL(204000000, static_cast<long>(buckets[0].costNano));
    LONGS_EQUAL(0, static_cast<long>(buckets[0].unpricedMessages));
    CHECK_FALSE(buckets[0].costOverflow);
}

TEST(ClaudeUsageStore, FastModeUsesFastRates) {
    ClaudeUsageRow r = makeRow("m1", "r1", epoch(2026, 10, 8, 12));
    r.speed = "fast";
    r.input = 1000; r.output = 2000; r.cacheRead = 300000; r.cacheWrite5m = 4000; r.cacheWrite1h = 10000;
    CHECK_TRUE(store->ingest({r}).ok);
    // 1000*8000 + 4000*10000 + 10000*16000 + 300000*400 + 2000*40000 = 408,000,000
    LONGS_EQUAL(408000000, static_cast<long>(all()[0].costNano));
}

TEST(ClaudeUsageStore, UnknownModelAndFastWithoutFastRatesAreUnpriced) {
    ClaudeUsageRow unknown = makeRow("m1", "r1", epoch(2026, 10, 8), "claude-opus-5-6");
    unknown.input = 100;
    ClaudeUsageRow fastNoRates = makeRow("m2", "r2", epoch(2026, 10, 8), "claude-sonnet-5");
    fastNoRates.speed = "fast"; fastNoRates.input = 100;
    ClaudeUsageRow dated = makeRow("m3", "r3", epoch(2026, 10, 8), "claude-haiku-4-5-20251001");
    dated.input = 100;
    CHECK_TRUE(store->ingest({unknown, fastNoRates, dated}).ok);
    auto buckets = all();
    LONGS_EQUAL(3, static_cast<long>(buckets.size()));
    for (const auto& b : buckets) {
        if (b.model == "claude-haiku-4-5-20251001") {
            LONGS_EQUAL(100000, static_cast<long>(b.costNano));   // 100 * 1000
            LONGS_EQUAL(0, static_cast<long>(b.unpricedMessages));
        } else {
            LONGS_EQUAL(0, static_cast<long>(b.costNano));
            LONGS_EQUAL(1, static_cast<long>(b.unpricedMessages));
            LONGS_EQUAL(100, static_cast<long>(b.input));   // tokens still counted
        }
    }
}

TEST(ClaudeUsageStore, CatalogChangeRecostsWithoutReingest) {
    ClaudeUsageRow r = makeRow("m1", "r1", epoch(2026, 10, 8));
    r.input = 1000;
    CHECK_TRUE(store->ingest({r}).ok);
    LONGS_EQUAL(4000000, static_cast<long>(all()[0].costNano));

    catalog["claude-opus-5-5"].inputNano = 6000;
    LONGS_EQUAL(6000000, static_cast<long>(all()[0].costNano));
    catalog.clear();
    auto none = all();
    LONGS_EQUAL(0, static_cast<long>(none[0].costNano));
    LONGS_EQUAL(1, static_cast<long>(none[0].unpricedMessages));
    LONGS_EQUAL(1, static_cast<long>(store->rowCount("acct")));
}

TEST(ClaudeUsageStore, TimeRangeIsHalfOpenAndAccountsAreIsolated) {
    ClaudeUsageRow a = makeRow("a", "a", 1000000000);
    ClaudeUsageRow b = makeRow("b", "b", 1000000100);
    ClaudeUsageRow other = makeRow("a", "a", 1000000000);
    other.account = "other";
    CHECK_TRUE(store->ingest({a, b, other}).ok);

    std::vector<ClaudeUsageBucket> out;
    std::string err;
    CHECK_TRUE(store->aggregate("acct", 1000000000, 1000000100, catalog, out, err));
    LONGS_EQUAL(1, static_cast<long>(ClaudeUsageStore::sum(out).messages));   // [from, to)
    CHECK_TRUE(store->aggregate("acct", 1000000000, 1000000101, catalog, out, err));
    LONGS_EQUAL(2, static_cast<long>(ClaudeUsageStore::sum(out).messages));
    CHECK_TRUE(store->aggregate("other", 0, 4102444800LL, catalog, out, err));
    LONGS_EQUAL(1, static_cast<long>(ClaudeUsageStore::sum(out).messages));
    CHECK_TRUE(store->aggregate("nobody", 0, 4102444800LL, catalog, out, err));
    LONGS_EQUAL(0, static_cast<long>(out.size()));
}

TEST(ClaudeUsageStore, SumAddsEveryField) {
    ClaudeUsageBucket a, b;
    a.messages = 1; a.input = 2; a.output = 3; a.cacheRead = 4; a.cacheWrite5m = 5; a.cacheWrite1h = 6;
    a.webSearch = 7; a.costNano = 8; a.unpricedMessages = 1;
    b = a;
    ClaudeUsageTotals t = ClaudeUsageStore::sum({a, b});
    LONGS_EQUAL(2, static_cast<long>(t.messages));
    LONGS_EQUAL(4, static_cast<long>(t.input));
    LONGS_EQUAL(12, static_cast<long>(t.cacheWrite1h));
    LONGS_EQUAL(16, static_cast<long>(t.costNano));
    LONGS_EQUAL(2, static_cast<long>(t.unpricedMessages));
    CHECK_FALSE(t.costOverflow);
}

TEST(ClaudeUsageStore, CostOverflowIsDetectedAndSaturates) {
    ModelPrice huge;
    huge.modelId = "claude-huge";
    huge.inputNano = huge.write5mNano = huge.write1hNano = huge.readNano = huge.outputNano = 999999999;
    std::map<std::string, ModelPrice> custom;
    custom["claude-huge"] = huge;
    ClaudeUsageRow r = makeRow("m", "r", epoch(2026, 10, 8), "claude-huge");
    r.input = r.output = r.cacheRead = r.cacheWrite5m = r.cacheWrite1h = ClaudeUsageStore::kMaxTokensPerField;
    ClaudeUsageRow r2 = r;
    r2.messageId = "m2";
    r2.requestId = "r2";
    // One such row is 5e18 nano-dollars (fits in int64); two rows are 1e19 (do not).
    CHECK_TRUE(store->ingest({r, r2}).ok);
    std::vector<ClaudeUsageBucket> out;
    std::string err;
    CHECK_TRUE(store->aggregate("acct", 0, 4102444800LL, custom, out, err));
    CHECK_TRUE(out[0].costOverflow);
    CHECK_TRUE(out[0].costNano == INT64_MAX);
    CHECK_TRUE(ClaudeUsageStore::sum(out).costOverflow);

    // realistic maximum: two rows of 1e9 tokens in every field at $100/MTok does not overflow
    custom["claude-huge"].inputNano = custom["claude-huge"].write5mNano = custom["claude-huge"].write1hNano =
        custom["claude-huge"].readNano = custom["claude-huge"].outputNano = 100000;
    CHECK_TRUE(store->aggregate("acct", 0, 4102444800LL, custom, out, err));
    CHECK_FALSE(out[0].costOverflow);
    LONGS_EQUAL(1000000000000000LL, static_cast<long>(out[0].costNano));  // 5 fields * 2e9 tokens * 1e5
}

TEST(ClaudeUsageStore, PersistsAcrossCloseAndReopenIncludingCursors) {
    ClaudeUsageRow r = makeRow("m1", "r1", epoch(2026, 10, 8));
    r.input = 42;
    CHECK_TRUE(store->ingest({r}).ok);
    ClaudeFileCursor c;
    c.account = "acct"; c.path = "p/s1.jsonl"; c.offset = 1234; c.size = 2000; c.mtime = 99;
    CHECK_TRUE(store->saveCursor(c));
    c.offset = 1500;
    CHECK_TRUE(store->saveCursor(c));    // update in place
    store->close();

    ClaudeUsageStore reopened;
    CHECK_TRUE(reopened.open(dbPath));
    LONGS_EQUAL(1, static_cast<long>(reopened.rowCount("acct")));
    auto cursors = reopened.loadCursors("acct");
    LONGS_EQUAL(1, static_cast<long>(cursors.size()));
    STRCMP_EQUAL("p/s1.jsonl", cursors[0].path.c_str());
    LONGS_EQUAL(1500, static_cast<long>(cursors[0].offset));
    LONGS_EQUAL(2000, static_cast<long>(cursors[0].size));
    LONGS_EQUAL(99, static_cast<long>(cursors[0].mtime));
    LONGS_EQUAL(0, static_cast<long>(reopened.loadCursors("other").size()));
}

TEST(ClaudeUsageStore, RetentionPrunesOldRowsOnly) {
    CHECK_TRUE(store->ingest({makeRow("old", "o", epoch(2025, 1, 1)),
                              makeRow("new", "n", epoch(2026, 10, 1))}).ok);
    LONGS_EQUAL(1, static_cast<long>(store->pruneOlderThan(epoch(2026, 1, 1))));
    LONGS_EQUAL(1, static_cast<long>(store->rowCount("acct")));
    LONGS_EQUAL(0, static_cast<long>(store->pruneOlderThan(epoch(2026, 1, 1))));
}

TEST(ClaudeUsageStore, ClosedAndUnopenableStoresFailGracefully) {
    ClaudeUsageStore closed;
    ClaudeIngestResult res = closed.ingest({makeRow("m", "r", epoch(2026, 10, 8))});
    CHECK_FALSE(res.ok);
    CHECK_TRUE(!res.error.empty());
    std::vector<ClaudeUsageBucket> out;
    std::string err;
    CHECK_FALSE(closed.aggregate("acct", 0, 1, catalog, out, err));
    LONGS_EQUAL(-1, static_cast<long>(closed.pruneOlderThan(1)));
    LONGS_EQUAL(0, static_cast<long>(closed.rowCount("acct")));
    CHECK_FALSE(closed.saveCursor(ClaudeFileCursor()));

    ClaudeUsageStore bad;
    CHECK_FALSE(bad.open("/proc/aimon_no_such_dir/history.db"));
}

TEST(ClaudeUsageStore, ConcurrentWritersAndReadersStayConsistent) {
    const int writers = 4;
    const int perWriter = 150;
    std::atomic<bool> stop(false);
    std::atomic<int> readerFailures(0);

    std::vector<std::thread> readers;
    for (int i = 0; i < 2; ++i) {
        readers.emplace_back([&]() {
            while (!stop.load()) {
                std::vector<ClaudeUsageBucket> out;
                std::string err;
                if (!store->aggregate("acct", 0, 4102444800LL, catalog, out, err)) {
                    ++readerFailures;
                }
                store->rowCount("acct");
            }
        });
    }
    std::vector<std::thread> threads;
    for (int w = 0; w < writers; ++w) {
        threads.emplace_back([&, w]() {
            for (int i = 0; i < perWriter; ++i) {
                ClaudeUsageRow r = makeRow("m" + std::to_string(w) + "_" + std::to_string(i),
                                           "r" + std::to_string(i), epoch(2026, 10, 8, 1, 0, 0) + i);
                r.input = 1;
                store->ingest({r});
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    stop.store(true);
    for (auto& t : readers) {
        t.join();
    }
    LONGS_EQUAL(0, readerFailures.load());
    LONGS_EQUAL(writers * perWriter, static_cast<long>(store->rowCount("acct")));
    LONGS_EQUAL(writers * perWriter, static_cast<long>(ClaudeUsageStore::sum(all()).input));
}

TEST_GROUP(ClaudeUsagePricing) {
    std::string dir;
    std::string dbPath;
    ClaudeUsageStore* store = nullptr;
    std::map<std::string, ModelPrice> v1;
    std::map<std::string, ModelPrice> v2;

    void setup() {
        dir = makeTempDir();
        CHECK_TRUE(!dir.empty());
        dbPath = dir + "/history.db";
        store = new ClaudeUsageStore();
        CHECK_TRUE(store->open(dbPath));
        v1 = realCatalog();
        v2 = catalogV2();
    }
    void teardown() {
        delete store;
        std::string cmd = "rm -rf '" + dir + "'";
        int rc = std::system(cmd.c_str());
        (void)rc;
    }
    int64_t costOnDay(const std::vector<PriceVersion>& versions, int day, bool* unpriced = nullptr) {
        std::vector<ClaudeUsageBucket> out;
        std::string err;
        CHECK_TRUE(store->aggregate("acct", epoch(2026, 10, day), epoch(2026, 10, day + 1), versions, out, err));
        LONGS_EQUAL(1, static_cast<long>(out.size()));
        if (unpriced) {
            *unpriced = out[0].unpricedMessages > 0;
        }
        return out[0].costNano;
    }
    ClaudeUsageRow haiku(int day, const char* id) {
        ClaudeUsageRow r = makeRow(id, id, epoch(2026, 10, day, 12), "claude-haiku-5-5");
        return r;
    }
};

TEST(ClaudeUsagePricing, PromptLengthSelectsTheRateSetForATieredModel) {
    PriceVersion ver;
    ver.models = v2;
    std::vector<PriceVersion> versions = {ver};

    ClaudeUsageRow a = haiku(8, "a");    // prompt exactly 100000 -> base rates
    a.input = 60000; a.cacheRead = 40000; a.output = 1000;
    ClaudeUsageRow b = haiku(9, "b");    // prompt 100001 -> upper rates
    b.input = 60001; b.cacheRead = 40000; b.output = 1000;
    ClaudeUsageRow c = haiku(10, "c");   // cache writes count toward the prompt
    c.input = 1; c.cacheWrite1h = 100000;
    ClaudeUsageRow d = haiku(11, "d");   // output tokens do not count toward the prompt
    d.input = 1; d.cacheWrite5m = 99999; d.output = 5000000;
    CHECK_TRUE(store->ingest({a, b, c, d}).ok);

    // a: 60000*100 + 40000*10 + 1000*500
    LONGS_EQUAL(6900000, static_cast<long>(costOnDay(versions, 8)));
    // b: 60001*500 + 40000*50 + 1000*2500
    LONGS_EQUAL(34500500, static_cast<long>(costOnDay(versions, 9)));
    // c: 1*500 + 100000*1000
    LONGS_EQUAL(100000500, static_cast<long>(costOnDay(versions, 10)));
    // d: 1*100 + 99999*125 + 5000000*500
    CHECK_TRUE(costOnDay(versions, 11) == 2512499975LL);
}

TEST(ClaudeUsagePricing, FlatModelsIgnorePromptLengthAndFastOnTieredIsUnpriced) {
    PriceVersion ver;
    ver.models = v2;
    std::vector<PriceVersion> versions = {ver};

    ClaudeUsageRow big = makeRow("m1", "r1", epoch(2026, 10, 8, 12), "claude-sonnet-5-5");
    big.cacheRead = 900000;                         // far over 100k: flat model, same rate
    ClaudeUsageRow fastHaiku = haiku(9, "f");
    fastHaiku.speed = "fast"; fastHaiku.input = 10;
    CHECK_TRUE(store->ingest({big, fastHaiku}).ok);

    LONGS_EQUAL(90000000, static_cast<long>(costOnDay(versions, 8)));   // 900000 * 100 (v2 read rate)
    bool unpriced = false;
    LONGS_EQUAL(0, static_cast<long>(costOnDay(versions, 9, &unpriced)));
    CHECK_TRUE(unpriced);
}

TEST(ClaudeUsagePricing, PriceVersionSelectedByMessageTime) {
    PriceVersion first;  first.effectiveFrom = 0;                             first.models = v1;
    PriceVersion second; second.effectiveFrom = epoch(2026, 10, 8, 6);        second.models = v2;
    ClaudeUsageRow before = makeRow("a", "a", epoch(2026, 10, 8, 5, 59, 59), "claude-sonnet-5-5");
    before.cacheRead = 1000000;
    ClaudeUsageRow at = makeRow("b", "b", epoch(2026, 10, 8, 6, 0, 0), "claude-sonnet-5-5");
    at.cacheRead = 1000000;
    CHECK_TRUE(store->ingest({before, at}).ok);

    std::vector<ClaudeUsageBucket> out;
    std::string err;
    CHECK_TRUE(store->aggregate("acct", epoch(2026, 10, 8, 5), epoch(2026, 10, 8, 6), std::vector<PriceVersion>{second, first}, out, err));
    LONGS_EQUAL(200000000, static_cast<long>(out[0].costNano));    // old read rate 200 (also: input order is irrelevant)
    CHECK_TRUE(store->aggregate("acct", epoch(2026, 10, 8, 6), epoch(2026, 10, 8, 7), std::vector<PriceVersion>{first, second}, out, err));
    LONGS_EQUAL(100000000, static_cast<long>(out[0].costNano));    // new read rate 100
    CHECK_TRUE(store->aggregate("acct", epoch(2026, 10, 8, 5), epoch(2026, 10, 8, 7), std::vector<PriceVersion>{first, second}, out, err));
    LONGS_EQUAL(300000000, static_cast<long>(out[0].costNano));    // one bucket, both versions
    LONGS_EQUAL(2, static_cast<long>(out[0].messages));
}

TEST(ClaudeUsagePricing, MessagesOlderThanEveryVersionUseTheFirstVersion) {
    PriceVersion only;
    only.effectiveFrom = epoch(2026, 10, 8, 6);
    only.models = v2;
    ClaudeUsageRow old = makeRow("a", "a", epoch(2026, 10, 1), "claude-sonnet-5-5");
    old.cacheRead = 1000000;
    CHECK_TRUE(store->ingest({old}).ok);
    std::vector<ClaudeUsageBucket> out;
    std::string err;
    CHECK_TRUE(store->aggregate("acct", 0, 4102444800LL, std::vector<PriceVersion>{only}, out, err));
    LONGS_EQUAL(100000000, static_cast<long>(out[0].costNano));
    CHECK_TRUE(store->aggregate("acct", 0, 4102444800LL, std::vector<PriceVersion>(), out, err));
    LONGS_EQUAL(0, static_cast<long>(out[0].costNano));            // no versions at all: unpriced
    LONGS_EQUAL(1, static_cast<long>(out[0].unpricedMessages));
}

TEST(ClaudeUsagePricing, CatalogVersionsPersistAndRevertsAreKept) {
    LONGS_EQUAL(static_cast<long>(VersionSave::Inserted), static_cast<long>(store->saveCatalogVersion(100, "sha-a", v1)));
    LONGS_EQUAL(static_cast<long>(VersionSave::Unchanged), static_cast<long>(store->saveCatalogVersion(150, "sha-a", v1)));
    LONGS_EQUAL(static_cast<long>(VersionSave::Inserted), static_cast<long>(store->saveCatalogVersion(200, "sha-b", v2)));
    LONGS_EQUAL(static_cast<long>(VersionSave::Unchanged), static_cast<long>(store->saveCatalogVersion(250, "sha-b", v2)));
    LONGS_EQUAL(static_cast<long>(VersionSave::Inserted), static_cast<long>(store->saveCatalogVersion(300, "sha-a", v1)));  // revert
    store->close();

    ClaudeUsageStore reopened;
    CHECK_TRUE(reopened.open(dbPath));
    std::vector<PriceVersion> versions;
    std::string err;
    CHECK_TRUE(reopened.loadCatalogVersions(versions, err));
    LONGS_EQUAL(3, static_cast<long>(versions.size()));
    LONGS_EQUAL(100, static_cast<long>(versions[0].effectiveFrom));
    STRCMP_EQUAL("sha-a", versions[0].pageSha256.c_str());
    LONGS_EQUAL(200, static_cast<long>(versions[1].effectiveFrom));
    LONGS_EQUAL(300, static_cast<long>(versions[2].effectiveFrom));
    CHECK_TRUE(versions[0].models == v1);
    CHECK_TRUE(versions[1].models == v2);
    CHECK_TRUE(versions[2].models == v1);
    CHECK_TRUE(versions[1].models.at("claude-haiku-5-5").upperTier != nullptr);   // tiers survive storage
}

TEST(ClaudeUsagePricing, VersionStorageFailsGracefully) {
    ClaudeUsageStore closed;
    LONGS_EQUAL(static_cast<long>(VersionSave::Failed), static_cast<long>(closed.saveCatalogVersion(1, "x", v1)));
    std::vector<PriceVersion> out;
    std::string err;
    CHECK_FALSE(closed.loadCatalogVersions(out, err));
    CHECK_TRUE(!err.empty());

    // a row with unparseable catalog JSON makes loading fail instead of silently skipping it
    store->close();
    sqlite3* raw = nullptr;
    CHECK_TRUE(sqlite3_open(dbPath.c_str(), &raw) == SQLITE_OK);
    CHECK_TRUE(sqlite3_exec(raw, "INSERT INTO claude_price_versions (fetched_at, page_sha256, catalog_json)"
                                 " VALUES (5, 'bad', 'not json');", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);
    ClaudeUsageStore reopened;
    CHECK_TRUE(reopened.open(dbPath));
    CHECK_FALSE(reopened.loadCatalogVersions(out, err));
    CHECK_TRUE(err.find("version") != std::string::npos);
    CHECK_TRUE(out.empty());
}

TEST_GROUP(ClaudeCycle) {};

TEST(ClaudeCycle, ResetDayOneWithinAMonth) {
    ClaudeCycle c;
    CHECK_TRUE(claudeCycleBounds(epoch(2026, 10, 8, 5), 1, c));
    LONGS_EQUAL(epoch(2026, 10, 1), static_cast<long>(c.startEpoch));
    LONGS_EQUAL(epoch(2026, 11, 1), static_cast<long>(c.endEpoch));
}

TEST(ClaudeCycle, BoundaryInstantsBelongToTheNewCycle) {
    ClaudeCycle c;
    CHECK_TRUE(claudeCycleBounds(epoch(2026, 11, 1, 0, 0, 0), 1, c));
    LONGS_EQUAL(epoch(2026, 11, 1), static_cast<long>(c.startEpoch));
    CHECK_TRUE(claudeCycleBounds(epoch(2026, 10, 31, 23, 59, 59), 1, c));
    LONGS_EQUAL(epoch(2026, 10, 1), static_cast<long>(c.startEpoch));
    LONGS_EQUAL(epoch(2026, 11, 1), static_cast<long>(c.endEpoch));
}

TEST(ClaudeCycle, MidMonthResetDayBeforeAndAfter) {
    ClaudeCycle c;
    CHECK_TRUE(claudeCycleBounds(epoch(2026, 10, 8), 15, c));
    LONGS_EQUAL(epoch(2026, 9, 15), static_cast<long>(c.startEpoch));
    LONGS_EQUAL(epoch(2026, 10, 15), static_cast<long>(c.endEpoch));
    CHECK_TRUE(claudeCycleBounds(epoch(2026, 10, 15), 15, c));
    LONGS_EQUAL(epoch(2026, 10, 15), static_cast<long>(c.startEpoch));
    LONGS_EQUAL(epoch(2026, 11, 15), static_cast<long>(c.endEpoch));
}

TEST(ClaudeCycle, YearWrapBothDirections) {
    ClaudeCycle c;
    CHECK_TRUE(claudeCycleBounds(epoch(2027, 1, 10), 20, c));
    LONGS_EQUAL(epoch(2026, 12, 20), static_cast<long>(c.startEpoch));
    LONGS_EQUAL(epoch(2027, 1, 20), static_cast<long>(c.endEpoch));
    CHECK_TRUE(claudeCycleBounds(epoch(2026, 12, 25), 20, c));
    LONGS_EQUAL(epoch(2026, 12, 20), static_cast<long>(c.startEpoch));
    LONGS_EQUAL(epoch(2027, 1, 20), static_cast<long>(c.endEpoch));
}

TEST(ClaudeCycle, LeapDayAndLastValidResetDay) {
    ClaudeCycle c;
    CHECK_TRUE(claudeCycleBounds(epoch(2028, 2, 29, 12), 28, c));       // leap day, reset day 28
    LONGS_EQUAL(epoch(2028, 2, 28), static_cast<long>(c.startEpoch));
    LONGS_EQUAL(epoch(2028, 3, 28), static_cast<long>(c.endEpoch));
    CHECK_TRUE(claudeCycleBounds(epoch(2028, 3, 1), 28, c));
    LONGS_EQUAL(epoch(2028, 2, 28), static_cast<long>(c.startEpoch));
    CHECK_TRUE(claudeCycleBounds(epoch(2027, 3, 1), 1, c));
    LONGS_EQUAL(epoch(2027, 3, 1), static_cast<long>(c.startEpoch));
    LONGS_EQUAL(epoch(2027, 4, 1), static_cast<long>(c.endEpoch));
}

TEST(ClaudeCycle, InvalidResetDayIsRejected) {
    ClaudeCycle c;
    CHECK_FALSE(claudeCycleBounds(epoch(2026, 10, 8), 0, c));
    CHECK_FALSE(claudeCycleBounds(epoch(2026, 10, 8), 29, c));
    CHECK_FALSE(claudeCycleBounds(epoch(2026, 10, 8), -3, c));
    CHECK_FALSE(claudeCycleBounds(-1, 1, c));
}

int main(int ac, char** av) {
    return CommandLineTestRunner::RunAllTests(ac, av);
}

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
