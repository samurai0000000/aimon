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

static void testTransientConnectionErrorRetryAndRetention() {
    std::cout << "[TestCursorCollector] Running testTransientConnectionErrorRetryAndRetention..." << std::endl;

    CursorConfig cfg;
    cfg.accessToken = "test_enterprise_token";
    CursorCollector collector(cfg);

    const std::string summaryBody = R"({
        "billingCycleStart": "2026-10-01T00:00:00.000Z",
        "billingCycleEnd": "2026-11-01T00:00:00.000Z",
        "membershipType": "enterprise",
        "individualUsage": {
            "overall": {
                "enabled": true,
                "used": 98134,
                "limit": 120000,
                "remaining": 21866
            }
        }
    })";

    const std::string spendBody = R"({
        "dailySpend": [
            {"day": 1790812800000, "spendCents": 45000.0, "category": "auto-grok-4.7-high"},
            {"day": 1790899200000, "spendCents": 53174.0, "category": "auto-claude-opus-5-5-medium"}
        ]
    })";

    const std::string prevSpendBody = R"({
        "dailySpend": [
            {"day": 1788220800000, "spendCents": 127231.0, "category": "auto-grok-4.7-high"}
        ]
    })";

    // Case 1: Initial poll fails all retries with connection error -> reports connection error
    int summaryCalls = 0;
    collector.setHttpRequestFn([&](const std::string&, const std::string& path,
                                   const std::vector<std::pair<std::string, std::string>>&,
                                   const std::string&) -> CursorHttpResponse {
        if (path == "/auth/usage-summary") {
            ++summaryCalls;
        }
        return CursorHttpResponse{false, 0, ""};
    });

    CursorStatus initialFail = collector.fetchStatus();
    assert(!initialFail.isAuthenticated);
    assert(summaryCalls == 2);
    assert(initialFail.errorMessage == "Cursor API request failed (status: connection error)");

    // Case 2: First attempt fails with connection error, retry succeeds -> full status populated
    summaryCalls = 0;
    int spendCalls = 0;
    collector.setHttpRequestFn([&](const std::string&, const std::string& path,
                                   const std::vector<std::pair<std::string, std::string>>&,
                                   const std::string&) -> CursorHttpResponse {
        if (path == "/auth/usage-summary") {
            ++summaryCalls;
            if (summaryCalls == 1) {
                return CursorHttpResponse{false, 0, ""};
            }
            return CursorHttpResponse{true, 200, summaryBody};
        }
        if (path == "/aiserver.v1.DashboardService/GetDailySpendByCategory") {
            ++spendCalls;
            return CursorHttpResponse{true, 200, (spendCalls == 1) ? spendBody : prevSpendBody};
        }
        return CursorHttpResponse{false, 0, ""};
    });

    CursorStatus okStatus = collector.fetchStatus();
    assert(okStatus.isAuthenticated);
    assert(summaryCalls == 2);
    assert(okStatus.planTier == "enterprise");
    assert(okStatus.fastRequestsUsed == 98134);
    assert(okStatus.fastRequestsLimit == 120000);
    assert(okStatus.usageMode == "spend");
    assert(okStatus.totalSpendUsd > 981.73 && okStatus.totalSpendUsd < 981.75);
    assert(okStatus.prevCycleSpendUsd > 1272.30 && okStatus.prevCycleSpendUsd < 1272.32);
    assert(okStatus.dailySpendHistory.size() == 2);
    assert(okStatus.categorySpendList.size() == 2);
    assert(okStatus.errorMessage.empty());

    // Case 3: Subsequent poll hits transient connection error across all retries -> retains last valid status
    collector.setHttpRequestFn([&](const std::string&, const std::string&,
                                   const std::vector<std::pair<std::string, std::string>>&,
                                   const std::string&) -> CursorHttpResponse {
        return CursorHttpResponse{false, 0, ""};
    });

    CursorStatus retainedOnConnErr = collector.fetchStatus();
    assert(retainedOnConnErr.isAuthenticated);
    assert(retainedOnConnErr.planTier == "enterprise");
    assert(retainedOnConnErr.fastRequestsUsed == 98134);
    assert(retainedOnConnErr.fastRequestsLimit == 120000);
    assert(retainedOnConnErr.dailySpendHistory.size() == 2);
    assert(retainedOnConnErr.errorMessage.empty());

    // Case 4: Subsequent poll succeeds on /auth/usage-summary (with updated used count)
    // but GetDailySpendByCategory fails with connection error -> retains spend breakdown
    const std::string updatedSummaryBody = R"({
        "billingCycleStart": "2026-10-01T00:00:00.000Z",
        "billingCycleEnd": "2026-11-01T00:00:00.000Z",
        "membershipType": "enterprise",
        "individualUsage": {
            "overall": {
                "enabled": true,
                "used": 98200,
                "limit": 120000,
                "remaining": 21800
            }
        }
    })";
    collector.setHttpRequestFn([&](const std::string&, const std::string& path,
                                   const std::vector<std::pair<std::string, std::string>>&,
                                   const std::string&) -> CursorHttpResponse {
        if (path == "/auth/usage-summary") {
            return CursorHttpResponse{true, 200, updatedSummaryBody};
        }
        return CursorHttpResponse{false, 0, ""};
    });

    CursorStatus partialSpendFail = collector.fetchStatus();
    assert(partialSpendFail.isAuthenticated);
    assert(partialSpendFail.fastRequestsUsed == 98200);
    assert(partialSpendFail.usageMode == "spend");
    assert(partialSpendFail.totalSpendUsd > 981.73 && partialSpendFail.totalSpendUsd < 981.75);
    assert(partialSpendFail.prevCycleSpendUsd > 1272.30 && partialSpendFail.prevCycleSpendUsd < 1272.32);
    assert(partialSpendFail.dailySpendHistory.size() == 2);
    assert(partialSpendFail.categorySpendList.size() == 2);

    // Case 5: Real 401 authentication failure clears cached authentication state
    collector.setHttpRequestFn([&](const std::string&, const std::string&,
                                   const std::vector<std::pair<std::string, std::string>>&,
                                   const std::string&) -> CursorHttpResponse {
        return CursorHttpResponse{true, 401, R"({"error":"unauthorized"})"};
    });

    CursorStatus authFail = collector.fetchStatus();
    assert(!authFail.isAuthenticated);
    assert(authFail.errorMessage == "Cursor API request failed (status: 401)");
}

int main() {
    std::cout << "=== Starting TestCursorCollector ===" << std::endl;
    testDailySpendPointSerialization();
    testHistoryDailyUsageSummary();
    testBoundaryAndEmptyCases();
    testCursorCollectorEnterpriseFallback();
    testTransientConnectionErrorRetryAndRetention();
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
