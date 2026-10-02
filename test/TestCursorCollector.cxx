/*
 * TestCursorCollector.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <cassert>
#include <iostream>
#include <filesystem>
#include <sqlite3.h>
#include "Models.hxx"
#include "HistoryStore.hxx"
#include "CursorCollector.hxx"

namespace fs = std::filesystem;
using namespace aimon;

static void testDailySpendPointSerialization() {
    std::cout << "[TestCursorCollector] Running testDailySpendPointSerialization..." << std::endl;
    DailySpendPoint pt;
    pt.dayMs = 1727827200000LL;
    pt.dayStr = "2026-10-02";
    pt.spendUsd = 0.0;
    pt.cumulativeUsd = 0.0;
    pt.requestsUsed = 706;
    pt.cumulativeRequests = 12206;

    nlohmann::json j = pt.toJson();
    assert(j.contains("day_ms") && j["day_ms"] == 1727827200000LL);
    assert(j.contains("day_str") && j["day_str"] == "2026-10-02");
    assert(j.contains("spend_usd") && j["spend_usd"] == 0.0);
    assert(j.contains("cumulative_usd") && j["cumulative_usd"] == 0.0);
    assert(j.contains("requests_used") && j["requests_used"] == 706);
    assert(j.contains("cumulative_requests") && j["cumulative_requests"] == 12206);

    CursorStatus cs;
    cs.planTier = "Enterprise";
    cs.usageMode = "requests";
    cs.fastRequestsUsed = 12206;
    cs.fastRequestsLimit = 120000;
    cs.dailySpendHistory.push_back(pt);

    nlohmann::json csJson = cs.toJson();
    assert(csJson.contains("usage_mode") && csJson["usage_mode"] == "requests");
    assert(csJson.contains("daily_spend") && csJson["daily_spend"].is_array());
    assert(csJson["daily_spend"].size() == 1);
    assert(csJson["daily_spend"][0]["requests_used"] == 706);
    assert(csJson["daily_spend"][0]["cumulative_requests"] == 12206);
}

static void testHistoryDailyUsageSummary() {
    std::cout << "[TestCursorCollector] Running testHistoryDailyUsageSummary..." << std::endl;
    std::string testDbPath = "/tmp/test_aimon_cursor_history.db";
    if (fs::exists(testDbPath)) {
        fs::remove(testDbPath);
    }

    HistoryStore store;
    assert(store.open(testDbPath));

    // Open raw sqlite to insert historical samples across 3 consecutive days
    sqlite3* db = nullptr;
    assert(sqlite3_open(testDbPath.c_str(), &db) == SQLITE_OK);

    std::tm tm1 = {};
    tm1.tm_year = 2026 - 1900;
    tm1.tm_mon = 8; // September
    tm1.tm_mday = 30;
    time_t day1 = timegm(&tm1);
    time_t day2 = day1 + 86400; // 2026-10-01
    time_t day3 = day2 + 86400; // 2026-10-02

    const char* insertSql =
        "INSERT INTO quota_samples (timestamp, provider, metric_key, metric_value, metric_limit, delta) VALUES "
        "(?, 'cursor', 'fast_requests_used', ?, 120000, 0);";

    auto insertRecord = [&](time_t ts, double val) {
        sqlite3_stmt* stmt = nullptr;
        assert(sqlite3_prepare_v2(db, insertSql, -1, &stmt, nullptr) == SQLITE_OK);
        sqlite3_bind_int64(stmt, 1, ts);
        sqlite3_bind_double(stmt, 2, val);
        assert(sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
    };

    // Day 1: two samples culminating in 10000
    insertRecord(day1 + 3600, 9500.0);
    insertRecord(day1 + 7200, 10000.0);

    // Day 2: two samples culminating in 11500
    insertRecord(day2 + 3600, 10800.0);
    insertRecord(day2 + 7200, 11500.0);

    // Day 3: two samples culminating in 12206
    insertRecord(day3 + 3600, 11900.0);
    insertRecord(day3 + 7200, 12206.0);

    sqlite3_close(db);

    // Query daily summary over the whole range
    std::vector<DailySpendPoint> pts = store.queryDailyUsageSummary("cursor", "fast_requests_used", day1, day3 + 86400);
    assert(pts.size() == 3);

    // Assert Point 1
    assert(pts[0].dayStr == "2026-09-30");
    assert(pts[0].requestsUsed == 10000);
    assert(pts[0].cumulativeRequests == 10000);
    assert(pts[0].spendUsd == 0.0);

    // Assert Point 2
    assert(pts[1].dayStr == "2026-10-01");
    assert(pts[1].requestsUsed == 1500); // 11500 - 10000
    assert(pts[1].cumulativeRequests == 11500);

    // Assert Point 3
    assert(pts[2].dayStr == "2026-10-02");
    assert(pts[2].requestsUsed == 706); // 12206 - 11500
    assert(pts[2].cumulativeRequests == 12206);

    store.close();
    if (fs::exists(testDbPath)) {
        fs::remove(testDbPath);
    }
}

static void testBoundaryAndEmptyCases() {
    std::cout << "[TestCursorCollector] Running testBoundaryAndEmptyCases..." << std::endl;
    std::string testDbPath = "/tmp/test_aimon_cursor_empty.db";
    if (fs::exists(testDbPath)) {
        fs::remove(testDbPath);
    }

    HistoryStore store;
    assert(store.open(testDbPath));

    // Empty DB query
    std::vector<DailySpendPoint> emptyPts = store.queryDailyUsageSummary("cursor", "fast_requests_used", 0, 2000000000);
    assert(emptyPts.empty());

    // Non-existent provider
    std::vector<DailySpendPoint> missingProvider = store.queryDailyUsageSummary("unknown", "fast_requests_used", 0, 2000000000);
    assert(missingProvider.empty());

    store.close();
    if (fs::exists(testDbPath)) {
        fs::remove(testDbPath);
    }
}

static void testCursorCollectorEnterpriseFallback() {
    std::cout << "[TestCursorCollector] Running testCursorCollectorEnterpriseFallback..." << std::endl;
    std::string testDbPath = "/tmp/test_aimon_cursor_collector_fallback.db";
    if (fs::exists(testDbPath)) {
        fs::remove(testDbPath);
    }

    HistoryStore store;
    assert(store.open(testDbPath));

    CursorConfig cfg;
    cfg.accessToken = "test_token";
    CursorCollector collector(cfg);
    collector.setHistoryStore(&store);

    CursorStatus status;
    status.isAuthenticated = true;
    status.planTier = "Enterprise";
    status.fastRequestsUsed = 12206;
    status.fastRequestsLimit = 120000;
    status.totalSpendUsd = 0.0;
    status.usageMode = "requests";

    DailySpendPoint pt;
    pt.dayStr = "2026-10-02";
    pt.requestsUsed = 706;
    pt.cumulativeRequests = 12206;
    status.dailySpendHistory.push_back(pt);

    nlohmann::json j = status.toJson();
    assert(j.contains("usage_mode") && j["usage_mode"] == "requests");
    assert(j.contains("fast_requests_used") && j["fast_requests_used"] == 12206);
    assert(j.contains("fast_requests_limit") && j["fast_requests_limit"] == 120000);
    assert(j.contains("daily_spend") && j["daily_spend"].size() == 1);
    assert(j["daily_spend"][0]["requests_used"] == 706);
    assert(j["daily_spend"][0]["cumulative_requests"] == 12206);

    store.close();
    if (fs::exists(testDbPath)) {
        fs::remove(testDbPath);
    }
}

int main() {
    std::cout << "=== Starting TestCursorCollector ===" << std::endl;
    testDailySpendPointSerialization();
    testHistoryDailyUsageSummary();
    testBoundaryAndEmptyCases();
    testCursorCollectorEnterpriseFallback();
    std::cout << "=== TestCursorCollector: ALL TESTS PASSED ===" << std::endl;
    return 0;
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
