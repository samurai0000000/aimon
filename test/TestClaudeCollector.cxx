/*
 * TestClaudeCollector.cxx
 *
 * Qualification tests for ClaudeCollector: incremental transcript reading,
 * deduplicated storage, windows and billing cycle, price versions, the price
 * refresh thread, privacy of stored data and fault handling. The only
 * stand-in is the price page fetch (a network boundary).
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "ClaudeCollector.hxx"
#include "ClaudeUsageStore.hxx"
#include "ConfigManager.hxx"
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
    tmv.tm_year = y - 1900; tmv.tm_mon = mo - 1; tmv.tm_mday = d;
    tmv.tm_hour = h; tmv.tm_min = mi; tmv.tm_sec = s;
    return static_cast<int64_t>(timegm(&tmv));
}

static std::string iso(int64_t e) {
    time_t t = static_cast<time_t>(e);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.123Z", tmv.tm_year + 1900, tmv.tm_mon + 1,
                  tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return buf;
}

static std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

static void writeFile(const std::string& path, const std::string& data, mode_t mode = 0644) {
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << data;
    }
    chmod(path.c_str(), mode);
}

static void appendFile(const std::string& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    out << data;
}

static void makeDirs(const std::string& path) {
    std::string cmd = "mkdir -p '" + path + "'";
    int rc = std::system(cmd.c_str());
    (void)rc;
}

// One transcript line with prompt text, a working directory and an identity
// attached, which must never reach the store.
static std::string usageLine(const std::string& msg, const std::string& req, int64_t ts, const std::string& model,
                             int64_t in, int64_t out, int64_t rd = 0, int64_t c5 = 0, int64_t c1 = 0,
                             const std::string& speed = "standard", bool sidechain = false) {
    nlohmann::json u = {{"input_tokens", in}, {"output_tokens", out}, {"cache_read_input_tokens", rd},
                        {"speed", speed},
                        {"cache_creation", {{"ephemeral_5m_input_tokens", c5}, {"ephemeral_1h_input_tokens", c1}}}};
    nlohmann::json j = {{"type", "assistant"},
                        {"message", {{"id", msg}, {"model", model}, {"role", "assistant"},
                                     {"content", "SECRET-PROMPT-MARKER-" + msg}, {"usage", u}}},
                        {"requestId", req}, {"timestamp", iso(ts)}, {"isSidechain", sidechain},
                        {"cwd", "/SECRET/CWD/MARKER"}, {"sessionId", "sess-1"},
                        {"userEmail", "user-marker@example.invalid"}};
    return j.dump() + "\n";
}

static PriceCatalog::FetchFn fetchOf(const std::string* body, std::atomic<int>* calls = nullptr, int status = 200) {
    return [body, calls, status](const std::string&) {
        if (calls) {
            ++(*calls);
        }
        PriceFetchResult r;
        r.status = status;
        r.body = *body;
        return r;
    };
}

TEST_GROUP(ClaudeCollectorGroup) {
    std::string root, cfgDir, projectDir, dbPath, pagesV1, pagesV2;
    std::string currentPage;
    ClaudeUsageStore* store = nullptr;
    PriceCatalog* catalog = nullptr;
    ClaudeConfig config;
    int64_t now = 0;

    void setup() {
        char tmpl[] = "/tmp/aimon_collector_test_XXXXXX";
        char* d = mkdtemp(tmpl);
        CHECK_TRUE(d != nullptr);
        root = d;
        cfgDir = root + "/cfg";
        projectDir = cfgDir + "/projects/proj-a";
        makeDirs(projectDir);
        dbPath = root + "/history.db";
        pagesV1 = readFile(std::string(AIMON_SOURCE_DIR) + "/test/fixtures/pricing_page.md");
        pagesV2 = readFile(std::string(AIMON_SOURCE_DIR) + "/test/fixtures/pricing_page_v2.md");
        currentPage = pagesV1;
        store = new ClaudeUsageStore();
        CHECK_TRUE(store->open(dbPath));
        catalog = new PriceCatalog(root + "/prices.json", "https://example.invalid/pricing.md", fetchOf(&currentPage));
        CHECK_TRUE(catalog->refresh(epoch(2026, 10, 8, 0)));
        ClaudeAccountConfig a;
        a.name = "acct";
        a.configDir = cfgDir;
        config.accounts = {a};
        now = epoch(2026, 10, 8, 12);
    }
    void teardown() {
        delete catalog;
        delete store;
        std::string cmd = "rm -rf '" + root + "'";
        int rc = std::system(cmd.c_str());
        (void)rc;
    }
    std::string transcript(const std::string& name = "s1.jsonl") { return projectDir + "/" + name; }
    ClaudeAccountStatus poll(ClaudeCollector& c, int64_t at = 0) {
        ClaudeStatus s = c.fetchStatus(at ? at : now);
        CHECK_TRUE(s.enabled);
        LONGS_EQUAL(static_cast<long>(config.resolvedAccounts().size()), static_cast<long>(s.accounts.size()));
        return s.accounts[0];
    }
    bool fileContains(const std::string& path, const std::string& needle) {
        return readFile(path).find(needle) != std::string::npos;
    }
};

TEST(ClaudeCollectorGroup, CollectsAndCostsTranscriptUsage) {
    writeFile(transcript(),
              usageLine("m1", "r1", now - 3600, "claude-sonnet-5-5", 1000, 2000, 3000, 400, 500) +
              usageLine("m2", "r2", now - 3000, "claude-opus-5-5", 10, 20, 30) +
              usageLine("m3", "r3", now - 2000, "claude-haiku-4-5-20251001", 100, 50));
    ClaudeCollector collector(config, *store, *catalog);
    ClaudeAccountStatus a = poll(collector);

    CHECK_TRUE(a.hasData);
    STRCMP_EQUAL("", a.errorMessage.c_str());
    STRCMP_EQUAL("acct", a.name.c_str());
    LONGS_EQUAL(3, static_cast<long>(a.window.messages));
    LONGS_EQUAL(1110, static_cast<long>(a.window.input));
    LONGS_EQUAL(2070, static_cast<long>(a.window.output));
    LONGS_EQUAL(3030, static_cast<long>(a.window.cacheRead));
    LONGS_EQUAL(400, static_cast<long>(a.window.cacheWrite5m));
    LONGS_EQUAL(500, static_cast<long>(a.window.cacheWrite1h));
    // sonnet 25,600,000 + opus 446,000 + haiku 350,000 nano-dollars
    LONGS_EQUAL(26396000, static_cast<long>(a.window.costNano));
    LONGS_EQUAL(0, static_cast<long>(a.window.unpricedMessages));
    LONGS_EQUAL(3, static_cast<long>(a.models.size()));
    LONGS_EQUAL(1, static_cast<long>(a.daily.size()));
    STRCMP_EQUAL("2026-10-08", a.daily[0].dayStr.c_str());
    LONGS_EQUAL(3, static_cast<long>(a.rowsInStore));
    LONGS_EQUAL(1, static_cast<long>(a.filesScanned));
    CHECK_TRUE(a.bytesReadLastPoll > 0);
}

TEST(ClaudeCollectorGroup, SecondPollReadsNoNewBytesAndAppendsAreIncremental) {
    std::string first = usageLine("m1", "r1", now - 100, "claude-opus-5-5", 10, 20);
    writeFile(transcript(), first);
    ClaudeCollector collector(config, *store, *catalog);
    ClaudeAccountStatus a = poll(collector);
    LONGS_EQUAL(static_cast<long>(first.size()), static_cast<long>(a.bytesReadLastPoll));

    ClaudeAccountStatus again = poll(collector);
    LONGS_EQUAL(0, static_cast<long>(again.bytesReadLastPoll));
    LONGS_EQUAL(static_cast<long>(a.window.costNano), static_cast<long>(again.window.costNano));

    std::string second = usageLine("m2", "r2", now - 50, "claude-opus-5-5", 5, 5);
    appendFile(transcript(), second);
    ClaudeAccountStatus appended = poll(collector);
    LONGS_EQUAL(static_cast<long>(second.size()), static_cast<long>(appended.bytesReadLastPoll));
    LONGS_EQUAL(2, static_cast<long>(appended.window.messages));
}

TEST(ClaudeCollectorGroup, PartialTrailingLineIsDeferredThenConsumed) {
    std::string line = usageLine("m1", "r1", now - 100, "claude-opus-5-5", 10, 20);
    std::string head = line.substr(0, line.size() / 2);
    std::string tail = line.substr(line.size() / 2);          // ends with the newline
    writeFile(transcript(), head);
    ClaudeCollector collector(config, *store, *catalog);
    ClaudeAccountStatus a = poll(collector);
    LONGS_EQUAL(0, static_cast<long>(a.window.messages));
    LONGS_EQUAL(0, static_cast<long>(a.skippedLines));         // deferred, not skipped

    appendFile(transcript(), tail);
    a = poll(collector);
    LONGS_EQUAL(1, static_cast<long>(a.window.messages));
    LONGS_EQUAL(0, static_cast<long>(a.skippedLines));

    // a complete line without its newline is also deferred until the newline arrives
    appendFile(transcript(), usageLine("m2", "r2", now - 90, "claude-opus-5-5", 1, 1).substr(0, 40));
    a = poll(collector);
    LONGS_EQUAL(1, static_cast<long>(a.window.messages));
}

TEST(ClaudeCollectorGroup, ShrunkOrReplacedFileIsReReadWithoutDoubleCounting) {
    std::string l1 = usageLine("m1", "r1", now - 100, "claude-opus-5-5", 10, 20);
    std::string l2 = usageLine("m2", "r2", now - 90, "claude-opus-5-5", 30, 40);
    writeFile(transcript(), l1 + l2);
    ClaudeCollector collector(config, *store, *catalog);
    ClaudeAccountStatus a = poll(collector);
    LONGS_EQUAL(2, static_cast<long>(a.window.messages));

    // replaced by a shorter file holding one new message
    std::string l3 = usageLine("m3", "r3", now - 80, "claude-opus-5-5", 50, 60);
    CHECK_TRUE(l3.size() < (l1 + l2).size());
    writeFile(transcript(), l3);
    a = poll(collector);
    LONGS_EQUAL(3, static_cast<long>(a.window.messages));      // m1, m2 kept; m3 added; nothing doubled
    LONGS_EQUAL(static_cast<long>(l3.size()), static_cast<long>(a.bytesReadLastPoll));
}

TEST(ClaudeCollectorGroup, RestartResumesFromPersistedCursors) {
    writeFile(transcript(), usageLine("m1", "r1", now - 100, "claude-opus-5-5", 10, 20) +
                            usageLine("m2", "r2", now - 90, "claude-opus-5-5", 30, 40));
    int64_t cost = 0;
    {
        ClaudeCollector collector(config, *store, *catalog);
        cost = poll(collector).window.costNano;
    }
    ClaudeUsageStore reopened;
    CHECK_TRUE(reopened.open(dbPath));
    ClaudeCollector restarted(config, reopened, *catalog);
    ClaudeAccountStatus a = poll(restarted);
    LONGS_EQUAL(0, static_cast<long>(a.bytesReadLastPoll));
    LONGS_EQUAL(2, static_cast<long>(a.window.messages));
    LONGS_EQUAL(static_cast<long>(cost), static_cast<long>(a.window.costNano));
}

TEST(ClaudeCollectorGroup, MissingAndUnreadableDirectoriesReportClearErrors) {
    ClaudeConfig bad;
    ClaudeAccountConfig a;
    a.name = "ghost";
    a.configDir = root + "/nonexistent";
    bad.accounts = {a};
    ClaudeCollector missing(bad, *store, *catalog);
    ClaudeAccountStatus s = missing.fetchStatus(now).accounts[0];
    CHECK_FALSE(s.hasData);
    CHECK_TRUE(!s.errorMessage.empty());

    makeDirs(root + "/empty-cfg");
    a.name = "empty";
    a.configDir = root + "/empty-cfg";
    bad.accounts = {a};
    ClaudeCollector noProjects(bad, *store, *catalog);
    s = noProjects.fetchStatus(now).accounts[0];
    CHECK_FALSE(s.hasData);
    CHECK_TRUE(s.errorMessage.find("projects") != std::string::npos);

    if (geteuid() != 0) {
        chmod((cfgDir + "/projects").c_str(), 0000);
        ClaudeCollector locked(config, *store, *catalog);
        s = locked.fetchStatus(now).accounts[0];
        chmod((cfgDir + "/projects").c_str(), 0755);
        CHECK_FALSE(s.hasData);
        CHECK_TRUE(!s.errorMessage.empty());
    }
}

TEST(ClaudeCollectorGroup, MalformedAndOversizedLinesAreSkippedAndCounted) {
    std::string big(ClaudeCollector::kMaxLineBytes + 4096, 'x');
    std::string text = usageLine("m1", "r1", now - 100, "claude-opus-5-5", 1, 1) +
                       "this is not json\n" + "{\"type\":\"assistant\",\"message\":{\"usage\":\n" +
                       big + "\n" +
                       usageLine("m2", "r2", now - 90, "claude-opus-5-5", 2, 2);
    // a usage-shaped line with an out-of-range value is skipped and counted too
    text += usageLine("m3", "r3", now - 80, "claude-opus-5-5", -5, 2);
    writeFile(transcript(), text);
    ClaudeCollector collector(config, *store, *catalog);
    ClaudeAccountStatus a = poll(collector);
    LONGS_EQUAL(2, static_cast<long>(a.window.messages));       // the two good lines around the junk
    CHECK_TRUE(a.skippedLines >= 2);                            // oversize line + out-of-range line
    LONGS_EQUAL(static_cast<long>(text.size()), static_cast<long>(a.bytesReadLastPoll));
}

TEST(ClaudeCollectorGroup, SubagentFilesAreCounted) {
    makeDirs(projectDir + "/sess/subagents");
    writeFile(transcript(), usageLine("m1", "r1", now - 100, "claude-opus-5-5", 1, 1));
    writeFile(projectDir + "/sess/subagents/agent-x.jsonl",
              usageLine("m9", "r9", now - 90, "claude-opus-5-5", 2, 2, 0, 0, 0, "standard", true));
    writeFile(projectDir + "/notes.txt", "not a transcript\n");
    ClaudeCollector collector(config, *store, *catalog);
    ClaudeAccountStatus a = poll(collector);
    LONGS_EQUAL(2, static_cast<long>(a.window.messages));
    LONGS_EQUAL(2, static_cast<long>(a.filesScanned));
}

TEST(ClaudeCollectorGroup, NoPromptTextWorkingDirectoryOrIdentityReachesTheDatabase) {
    writeFile(transcript(), usageLine("m1", "r1", now - 100, "claude-opus-5-5", 10, 20));
    writeFile(cfgDir + "/.credentials.json",
              "{\"claudeAiOauth\":{\"accessToken\":\"SECRET-ACCESS-TOKEN-XYZ\",\"refreshToken\":\"SECRET-REFRESH-TOKEN-XYZ\","
              "\"subscriptionType\":\"enterprise\",\"rateLimitTier\":\"tier-fixture\"}}", 0600);
    ClaudeCollector collector(config, *store, *catalog);
    ClaudeStatus status = collector.fetchStatus(now);
    std::string statusJson = status.toJson().dump();
    CHECK_TRUE(statusJson.find("SECRET") == std::string::npos);
    CHECK_TRUE(statusJson.find("user-marker") == std::string::npos);
    CHECK_TRUE(statusJson.find("/SECRET/CWD") == std::string::npos);
    store->close();                                             // flush into the main file
    const char* forbidden[] = {"SECRET-PROMPT-MARKER", "/SECRET/CWD/MARKER", "user-marker@example.invalid",
                               "SECRET-ACCESS-TOKEN-XYZ", "SECRET-REFRESH-TOKEN-XYZ"};
    for (const char* needle : forbidden) {
        CHECK_FALSE(fileContains(dbPath, needle));
        CHECK_FALSE(fileContains(dbPath + "-wal", needle));
        CHECK_FALSE(fileContains(root + "/prices.json", needle));
    }
}

TEST(ClaudeCollectorGroup, TierComesFromCredentialsAndAbsentCredentialsAreUnknown) {
    writeFile(transcript(), usageLine("m1", "r1", now - 100, "claude-opus-5-5", 1, 1));
    ClaudeCollector collector(config, *store, *catalog);
    ClaudeAccountStatus a = poll(collector);
    CHECK_TRUE(a.tier == ClaudeTier::Unknown);
    CHECK_TRUE(a.hasData);                                      // usage does not depend on credentials

    writeFile(cfgDir + "/.credentials.json",
              "{\"claudeAiOauth\":{\"accessToken\":\"SECRET-ACCESS-TOKEN-XYZ\",\"subscriptionType\":\"enterprise\","
              "\"rateLimitTier\":\"tier-fixture\"}}", 0600);
    a = poll(collector);
    CHECK_TRUE(a.tier == ClaudeTier::Enterprise);
    STRCMP_EQUAL("enterprise", a.rawSubscriptionType.c_str());
    STRCMP_EQUAL("tier-fixture", a.rawRateLimitTier.c_str());

    writeFile(cfgDir + "/.credentials.json",
              "{\"claudeAiOauth\":{\"accessToken\":\"SECRET-ACCESS-TOKEN-XYZ\",\"subscriptionType\":\"personal-fixture\"}}", 0600);
    a = poll(collector);                                        // a /login switch is picked up on the next poll
    CHECK_TRUE(a.tier == ClaudeTier::Personal);
    STRCMP_EQUAL("personal-fixture", a.rawSubscriptionType.c_str());
}

TEST(ClaudeCollectorGroup, WindowsAreComputedFromTheGivenTime) {
    auto haiku = [&](const char* id, int64_t ts) {
        return usageLine(id, id, ts, "claude-haiku-4-5", 1000, 0);
    };
    writeFile(transcript(),
              haiku("a", now - 3600) +                  // 11:00: in 5h, 7d, window, today
              haiku("b", now - 6 * 3600) +              // 06:00: in 7d, window, today
              haiku("f", epoch(2026, 10, 8, 0, 30)) +   // 00:30: in 7d, window, today
              haiku("c", now - 8 * 86400) +             // in the 30 day window only
              haiku("d", now - 40 * 86400) +            // outside the window
              haiku("e", now + 600));                   // in the future: excluded everywhere
    ClaudeCollector collector(config, *store, *catalog);
    ClaudeAccountStatus a = poll(collector);
    LONGS_EQUAL(1, static_cast<long>(a.last5h.messages));
    LONGS_EQUAL(3, static_cast<long>(a.last7d.messages));
    LONGS_EQUAL(4, static_cast<long>(a.window.messages));
    LONGS_EQUAL(3, static_cast<long>(a.today.messages));
    LONGS_EQUAL(1000000, static_cast<long>(a.last5h.costNano));    // 1000 tokens * 1000 nano
    LONGS_EQUAL(4000000, static_cast<long>(a.window.costNano));
    LONGS_EQUAL(30, a.windowDays);
    CHECK_FALSE(a.cycleConfigured);
    LONGS_EQUAL(6, static_cast<long>(store->rowCount("acct")));      // every valid line is stored, windows only filter
}

TEST(ClaudeCollectorGroup, BillingCycleSpendLimitAndPercent) {
    config.accounts[0].spendLimitUsd = 500;
    config.accounts[0].cycleResetDay = 1;
    auto sonnet = [&](const char* id, int64_t ts) {
        return usageLine(id, id, ts, "claude-sonnet-5-5", 0, 0, 1000000);   // 1M cache reads = 200,000,000 nano
    };
    writeFile(transcript(), sonnet("a", epoch(2026, 10, 2, 10)) + sonnet("b", epoch(2026, 10, 7, 10)) +
                            sonnet("old", epoch(2026, 9, 30, 23, 59, 59)));
    ClaudeCollector collector(config, *store, *catalog);
    ClaudeAccountStatus a = poll(collector);
    CHECK_TRUE(a.cycleConfigured);
    LONGS_EQUAL(2, static_cast<long>(a.cycle.messages));
    LONGS_EQUAL(400000000, static_cast<long>(a.cycle.costNano));
    LONGS_EQUAL(static_cast<long>(epoch(2026, 10, 1)), static_cast<long>(a.cycle.fromEpoch));
    STRCMP_EQUAL("2026-11-01T00:00:00Z", a.cycleResetIso.c_str());
    DOUBLES_EQUAL(500.0, a.spendLimitUsd, 1e-9);
    DOUBLES_EQUAL(0.08, a.estPctOfLimit, 1e-9);                 // $0.40 of $500

    config.accounts[0].spendLimitUsd = 0;                       // cycle only, no limit: no percentage
    ClaudeCollector noLimit(config, *store, *catalog);
    a = poll(noLimit);
    CHECK_TRUE(a.cycleConfigured);
    DOUBLES_EQUAL(0.0, a.estPctOfLimit, 1e-12);
}

TEST(ClaudeCollectorGroup, UnknownModelsAreUnpricedButCounted) {
    writeFile(transcript(), usageLine("m1", "r1", now - 100, "claude-future-9", 123, 7) +
                            usageLine("m2", "r2", now - 90, "claude-opus-5-5", 1, 1));
    ClaudeCollector collector(config, *store, *catalog);
    ClaudeAccountStatus a = poll(collector);
    LONGS_EQUAL(2, static_cast<long>(a.window.messages));
    LONGS_EQUAL(1, static_cast<long>(a.window.unpricedMessages));
    LONGS_EQUAL(1, static_cast<long>(a.unpricedModels.size()));
    STRCMP_EQUAL("claude-future-9", a.unpricedModels[0].c_str());
    bool found = false;
    for (const auto& m : a.models) {
        if (m.model == "claude-future-9") {
            found = true;
            LONGS_EQUAL(123, static_cast<long>(m.input));       // tokens are counted even without a price
            LONGS_EQUAL(0, static_cast<long>(m.costNano));
        }
    }
    CHECK_TRUE(found);
}

TEST(ClaudeCollectorGroup, TwoAccountsStaySeparate) {
    makeDirs(root + "/cfg2/projects/p");
    ClaudeAccountConfig b;
    b.name = "personal";
    b.configDir = root + "/cfg2";
    config.accounts.push_back(b);
    writeFile(transcript(), usageLine("m1", "r1", now - 100, "claude-opus-5-5", 10, 20));
    writeFile(root + "/cfg2/projects/p/x.jsonl", usageLine("m1", "r1", now - 100, "claude-opus-5-5", 1000, 2000) +
                                                 usageLine("m2", "r2", now - 90, "claude-opus-5-5", 1, 1));
    ClaudeCollector collector(config, *store, *catalog);
    ClaudeStatus s = collector.fetchStatus(now);
    LONGS_EQUAL(2, static_cast<long>(s.accounts.size()));
    STRCMP_EQUAL("acct", s.accounts[0].name.c_str());
    STRCMP_EQUAL("personal", s.accounts[1].name.c_str());
    LONGS_EQUAL(1, static_cast<long>(s.accounts[0].window.messages));      // same ids, different accounts
    LONGS_EQUAL(2, static_cast<long>(s.accounts[1].window.messages));
    LONGS_EQUAL(10, static_cast<long>(s.accounts[0].window.input));
    LONGS_EQUAL(1001, static_cast<long>(s.accounts[1].window.input));
}

TEST(ClaudeCollectorGroup, PriceVersionsAreRecordedOncePerCatalogChange) {
    auto sonnet = [&](const char* id, int64_t ts) {
        return usageLine(id, id, ts, "claude-sonnet-5-5", 0, 0, 1000000);
    };
    writeFile(transcript(), sonnet("m1", epoch(2026, 10, 8, 5)) + sonnet("m2", epoch(2026, 10, 8, 7)));
    ClaudeCollector collector(config, *store, *catalog);
    ClaudeStatus first = collector.fetchStatus(now);
    LONGS_EQUAL(1, static_cast<long>(first.pricing.versionCount));
    LONGS_EQUAL(400000000, static_cast<long>(first.accounts[0].window.costNano));   // old read rate 200
    ClaudeStatus again = collector.fetchStatus(now);
    LONGS_EQUAL(1, static_cast<long>(again.pricing.versionCount));                  // unchanged: no new version

    currentPage = pagesV2;                                                          // Anthropic changed the page
    CHECK_TRUE(catalog->refresh(epoch(2026, 10, 8, 6)));
    ClaudeStatus second = collector.fetchStatus(now);
    LONGS_EQUAL(2, static_cast<long>(second.pricing.versionCount));
    // m1 (05:00) keeps the old rate, m2 (07:00) uses the new one: 200M + 100M
    LONGS_EQUAL(300000000, static_cast<long>(second.accounts[0].window.costNano));
    CHECK_FALSE(second.pricing.stale);
    LONGS_EQUAL(20, static_cast<long>(second.pricing.modelsLoaded));
}

TEST(ClaudeCollectorGroup, WithoutAnyPricesEverythingIsUnpricedAndSaysWhy) {
    PriceCatalog empty(root + "/no-prices.json", "https://example.invalid/p.md", fetchOf(&currentPage, nullptr, 500));
    writeFile(transcript(), usageLine("m1", "r1", now - 100, "claude-opus-5-5", 10, 20));
    ClaudeCollector collector(config, *store, empty);
    ClaudeStatus s = collector.fetchStatus(now);
    LONGS_EQUAL(0, static_cast<long>(s.pricing.versionCount));
    CHECK_TRUE(!s.pricing.error.empty());
    LONGS_EQUAL(1, static_cast<long>(s.accounts[0].window.messages));
    LONGS_EQUAL(0, static_cast<long>(s.accounts[0].window.costNano));
    LONGS_EQUAL(1, static_cast<long>(s.accounts[0].window.unpricedMessages));
}

TEST(ClaudeCollectorGroup, RetentionRemovesOldRowsAndTheyAreNotReReadAfterwards) {
    config.retentionDays = 30;
    writeFile(transcript(), usageLine("old", "o", now - 100 * 86400, "claude-opus-5-5", 1, 1) +
                            usageLine("new", "n", now - 86400, "claude-opus-5-5", 2, 2));
    ClaudeCollector collector(config, *store, *catalog);
    ClaudeAccountStatus a = poll(collector);
    LONGS_EQUAL(1, static_cast<long>(store->rowCount("acct")));
    LONGS_EQUAL(1, static_cast<long>(a.window.messages));
    a = poll(collector, now + 7200);
    LONGS_EQUAL(1, static_cast<long>(store->rowCount("acct")));    // the old line is not re-ingested
    LONGS_EQUAL(0, static_cast<long>(a.bytesReadLastPoll));
}

TEST(ClaudeCollectorGroup, LargeTreeIsReadOnceAndThenCostsNothing) {
    for (int f = 0; f < 200; ++f) {
        std::string dir = cfgDir + "/projects/bulk-" + std::to_string(f % 20);
        makeDirs(dir);
        std::string text;
        for (int i = 0; i < 100; ++i) {
            std::string id = "b" + std::to_string(f) + "_" + std::to_string(i);
            text += usageLine(id, id, now - 1000 - i, "claude-haiku-4-5", 10, 5);
        }
        writeFile(dir + "/s" + std::to_string(f) + ".jsonl", text);
    }
    ClaudeCollector collector(config, *store, *catalog);
    auto t0 = std::chrono::steady_clock::now();
    ClaudeAccountStatus a = poll(collector);
    double firstSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    LONGS_EQUAL(20000, static_cast<long>(a.window.messages));
    CHECK_TRUE(firstSec < 60.0);

    t0 = std::chrono::steady_clock::now();
    a = poll(collector);
    double secondSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    LONGS_EQUAL(0, static_cast<long>(a.bytesReadLastPoll));
    CHECK_TRUE(secondSec < 5.0);
}

//------------------------------------------------------------------------
// Price refresh thread
//------------------------------------------------------------------------

TEST(ClaudeCollectorGroup, PriceRefreshFetchesWhenMissingStopsPromptlyAndDoesNotSpin) {
    std::atomic<int> calls(0);
    PriceCatalog fresh(root + "/fresh-prices.json", "https://example.invalid/p.md", fetchOf(&currentPage, &calls));
    {
        ClaudeCollector collector(config, *store, fresh);
        collector.startPriceRefresh(std::chrono::milliseconds(10));
        for (int i = 0; i < 300 && calls.load() < 1; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        LONGS_EQUAL(1, calls.load());
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        LONGS_EQUAL(1, calls.load());                              // fresh: no further fetches
        CHECK_TRUE(fresh.snapshot().size() == 19);

        auto t0 = std::chrono::steady_clock::now();
        collector.stopPriceRefresh();
        double stopSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK_TRUE(stopSec < 1.0);
        collector.stopPriceRefresh();                              // idempotent
    }
}

TEST(ClaudeCollectorGroup, FailedPriceRefreshBacksOffInsteadOfRetryingEveryTick) {
    std::atomic<int> calls(0);
    PriceCatalog failing(root + "/failing-prices.json", "https://example.invalid/p.md", fetchOf(&currentPage, &calls, 503));
    ClaudeCollector collector(config, *store, failing);
    collector.startPriceRefresh(std::chrono::milliseconds(10));
    for (int i = 0; i < 300 && calls.load() < 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    LONGS_EQUAL(1, calls.load());
    CHECK_TRUE(failing.snapshot().empty());
    // destructor stops the thread
}

//------------------------------------------------------------------------
// Line parsing
//------------------------------------------------------------------------

TEST_GROUP(ClaudeLineParser) {
    static ClaudeLineResult parse(const std::string& line, ClaudeUsageRow& row) {
        return ClaudeCollector::parseLine(line, "acct", row);
    }
    static std::string good() {
        return usageLine("m1", "r1", epoch(2026, 10, 8, 12), "claude-opus-5-5", 10, 20, 30, 40, 50, "fast", true);
    }
};

TEST(ClaudeLineParser, ParsesEveryFieldOfAUsageLine) {
    ClaudeUsageRow row;
    CHECK_TRUE(parse(good(), row) == ClaudeLineResult::Row);
    STRCMP_EQUAL("acct", row.account.c_str());
    STRCMP_EQUAL("sess-1", row.sessionId.c_str());
    STRCMP_EQUAL("m1", row.messageId.c_str());
    STRCMP_EQUAL("r1", row.requestId.c_str());
    STRCMP_EQUAL("claude-opus-5-5", row.model.c_str());
    STRCMP_EQUAL("fast", row.speed.c_str());
    LONGS_EQUAL(static_cast<long>(epoch(2026, 10, 8, 12)), static_cast<long>(row.timestamp));
    LONGS_EQUAL(10, static_cast<long>(row.input));
    LONGS_EQUAL(20, static_cast<long>(row.output));
    LONGS_EQUAL(30, static_cast<long>(row.cacheRead));
    LONGS_EQUAL(40, static_cast<long>(row.cacheWrite5m));
    LONGS_EQUAL(50, static_cast<long>(row.cacheWrite1h));
    CHECK_TRUE(row.sidechain);
    CHECK_FALSE(row.creationSplitMissing);
}

TEST(ClaudeLineParser, CacheCreationWithoutASplitIsCountedAsFiveMinute) {
    ClaudeUsageRow row;
    std::string line = "{\"type\":\"assistant\",\"message\":{\"id\":\"m\",\"model\":\"claude-opus-5-5\",\"usage\":"
                       "{\"input_tokens\":1,\"output_tokens\":2,\"cache_creation_input_tokens\":40}},"
                       "\"requestId\":\"r\",\"timestamp\":\"2026-10-08T12:00:00Z\"}";
    CHECK_TRUE(parse(line, row) == ClaudeLineResult::Row);
    LONGS_EQUAL(40, static_cast<long>(row.cacheWrite5m));
    LONGS_EQUAL(0, static_cast<long>(row.cacheWrite1h));
    CHECK_TRUE(row.creationSplitMissing);
    STRCMP_EQUAL("standard", row.speed.c_str());
    STRCMP_EQUAL("unknown", row.sessionId.c_str());
    CHECK_FALSE(row.sidechain);
}

TEST(ClaudeLineParser, NonUsageLinesAreIgnored) {
    ClaudeUsageRow row;
    CHECK_TRUE(parse("", row) == ClaudeLineResult::Ignored);
    CHECK_TRUE(parse("not json", row) == ClaudeLineResult::Ignored);
    CHECK_TRUE(parse("[1,2]", row) == ClaudeLineResult::Ignored);
    CHECK_TRUE(parse("{\"type\":\"user\",\"message\":{\"role\":\"user\"}}", row) == ClaudeLineResult::Ignored);
    CHECK_TRUE(parse("{\"type\":\"assistant\",\"message\":{\"id\":\"m\"}}", row) == ClaudeLineResult::Ignored);
    CHECK_TRUE(parse("{\"type\":\"assistant\",\"message\":\"text\"}", row) == ClaudeLineResult::Ignored);
    std::string synthetic = usageLine("m", "r", epoch(2026, 10, 8), "<synthetic>", 1, 1);
    CHECK_TRUE(parse(synthetic, row) == ClaudeLineResult::Ignored);
}

TEST(ClaudeLineParser, InvalidFieldsAreSkippedNotIgnored) {
    ClaudeUsageRow row;
    auto base = []() {
        return nlohmann::json::parse(usageLine("m", "r", epoch(2026, 10, 8, 12), "claude-opus-5-5", 1, 2));
    };
    nlohmann::json j;
    j = base(); j.erase("requestId");                          CHECK_TRUE(parse(j.dump(), row) == ClaudeLineResult::Skipped);
    j = base(); j["requestId"] = "";                           CHECK_TRUE(parse(j.dump(), row) == ClaudeLineResult::Skipped);
    j = base(); j["message"]["id"] = std::string(257, 'a');    CHECK_TRUE(parse(j.dump(), row) == ClaudeLineResult::Skipped);
    j = base(); j["message"]["model"] = "";                    CHECK_TRUE(parse(j.dump(), row) == ClaudeLineResult::Skipped);
    j = base(); j["timestamp"] = "yesterday";                  CHECK_TRUE(parse(j.dump(), row) == ClaudeLineResult::Skipped);
    j = base(); j["timestamp"] = "2026-13-08T12:00:00Z";       CHECK_TRUE(parse(j.dump(), row) == ClaudeLineResult::Skipped);
    j = base(); j["timestamp"] = "2026-10-08T25:00:00Z";       CHECK_TRUE(parse(j.dump(), row) == ClaudeLineResult::Skipped);
    j = base(); j["timestamp"] = "2026-10-08T12:00:00+00:00";  CHECK_TRUE(parse(j.dump(), row) == ClaudeLineResult::Skipped);
    j = base(); j["timestamp"] = "2100-01-01T00:00:00Z";       CHECK_TRUE(parse(j.dump(), row) == ClaudeLineResult::Skipped);
    j = base(); j["timestamp"] = "1970-01-01T00:00:00Z";       CHECK_TRUE(parse(j.dump(), row) == ClaudeLineResult::Skipped);
    j = base(); j["message"]["usage"]["input_tokens"] = -1;    CHECK_TRUE(parse(j.dump(), row) == ClaudeLineResult::Skipped);
    j = base(); j["message"]["usage"]["output_tokens"] = 1000000001LL; CHECK_TRUE(parse(j.dump(), row) == ClaudeLineResult::Skipped);
    j = base(); j["message"]["usage"]["cache_read_input_tokens"] = -7; CHECK_TRUE(parse(j.dump(), row) == ClaudeLineResult::Skipped);
    j = base(); j["message"]["usage"]["cache_creation"] = {{"ephemeral_1h_input_tokens", 2000000000LL}};
    CHECK_TRUE(parse(j.dump(), row) == ClaudeLineResult::Skipped);
    j = base();                                                CHECK_TRUE(parse(j.dump(), row) == ClaudeLineResult::Row);
}

TEST(ClaudeLineParser, TimestampsWithAndWithoutFractionsParseToTheSameSecond) {
    ClaudeUsageRow a, b;
    nlohmann::json j = nlohmann::json::parse(usageLine("m", "r", epoch(2026, 10, 8, 12), "claude-opus-5-5", 1, 2));
    j["timestamp"] = "2026-10-08T12:00:00Z";
    CHECK_TRUE(parse(j.dump(), a) == ClaudeLineResult::Row);
    j["timestamp"] = "2026-10-08T12:00:00.999999Z";
    CHECK_TRUE(parse(j.dump(), b) == ClaudeLineResult::Row);
    LONGS_EQUAL(static_cast<long>(a.timestamp), static_cast<long>(b.timestamp));
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
