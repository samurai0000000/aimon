/*
 * CursorCollector.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "CursorCollector.hxx"
#include "PathUtils.hxx"
#include <iostream>
#include <filesystem>
#include <map>
#include <algorithm>
#include <ctime>
#include <cstdio>
#include <chrono>
#include <fstream>
#include <limits>
#include <thread>
#include "HistoryStore.hxx"
#include <sqlite3.h>
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include <httplib.h>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace aimon {

static int64_t parseIsoToEpochMs(const std::string& iso) {
    if (iso.empty()) return 0;
    std::tm tm = {};
    if (strptime(iso.c_str(), "%Y-%m-%dT%H:%M:%S", &tm)) {
        time_t t = timegm(&tm);
        if (t > 0) return static_cast<int64_t>(t) * 1000;
    }
    return 0;
}

static std::string formatEpochMsToDate(int64_t ms) {
    time_t t = static_cast<time_t>(ms / 1000);
    std::tm tm = {};
    gmtime_r(&t, &tm);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

CursorCollector::CursorCollector(const CursorConfig& config)
    : _config(config) {
}

void CursorCollector::setHistoryStore(HistoryStore* historyStore) {
    std::lock_guard<std::mutex> lock(_mutex);
    _historyStore = historyStore;
}

void CursorCollector::setHttpRequestFn(HttpRequestFn fn) {
    std::lock_guard<std::mutex> lock(_mutex);
    _httpRequestFn = std::move(fn);
}

static bool isTransientHttpFailure(const CursorHttpResponse& resp) {
    return !resp.connected || resp.status == 408 || resp.status == 429 || resp.status >= 500;
}

CursorHttpResponse CursorCollector::performRequest(
    const std::string& method,
    const std::string& path,
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::string& body) {
    if (_httpRequestFn) {
        return _httpRequestFn(method, path, headers, body);
    }

    CursorHttpResponse out;
    try {
        httplib::SSLClient cli("api2.cursor.sh", 443);
        cli.set_connection_timeout(8, 0);
        cli.set_read_timeout(10, 0);
        cli.set_write_timeout(8, 0);

        httplib::Headers reqHeaders;
        for (const auto& kv : headers) {
            if (method == "POST" && kv.first == "Content-Type") {
                continue;
            }
            reqHeaders.emplace(kv.first, kv.second);
        }

        httplib::Result res = (method == "POST")
            ? cli.Post(path, reqHeaders, body, "application/json")
            : cli.Get(path, reqHeaders);

        if (res) {
            out.connected = true;
            out.status = res->status;
            out.body = res->body;
        }
    } catch (...) {
        out.connected = false;
    }
    return out;
}

std::string CursorCollector::extractTokenFromDb(const std::string& dbPath, std::string& outTier) {
    std::string token;
    if (dbPath.empty() || !fs::exists(dbPath)) {
        return token;
    }

    sqlite3* db = nullptr;
    int rc = sqlite3_open_v2(dbPath.c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return token;
    }

    auto queryKey = [&](const std::string& key) -> std::string {
        std::string val;
        const char* sql = "SELECT value FROM ItemTable WHERE key = ? LIMIT 1;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const unsigned char* text = sqlite3_column_text(stmt, 0);
                if (text) {
                    val = reinterpret_cast<const char*>(text);
                }
            }
            sqlite3_finalize(stmt);
        }
        return val;
    };

    token = queryKey("cursorAuth/accessToken");
    outTier = queryKey("cursorAuth/stripeMembershipType");

    sqlite3_close(db);
    return token;
}

std::string CursorCollector::resolveAccessToken() {
    if (!_config.accessToken.empty()) {
        return _config.accessToken;
    }

    const char* envToken = std::getenv("CURSOR_ACCESS_TOKEN");
    if (!envToken) {
        envToken = std::getenv("CURSOR_API_KEY");
    }
    if (envToken && *envToken) {
        return envToken;
    }

    // Try reading directly from config file in case it was updated at runtime
    std::string configPath = PathUtils::expandHome("~/.config/aimon/config.json");
    if (fs::exists(configPath)) {
        try {
            std::ifstream f(configPath);
            if (f.is_open()) {
                nlohmann::json c = nlohmann::json::parse(f, nullptr, false);
                if (!c.is_discarded() && c.contains("cursor") && c["cursor"].is_object()) {
                    if (c["cursor"].contains("access_token") && c["cursor"]["access_token"].is_string()) {
                        std::string t = c["cursor"]["access_token"].get<std::string>();
                        if (!t.empty()) {
                            _config.accessToken = t;
                            return t;
                        }
                    }
                }
            }
        } catch (...) {
        }
    }

    std::string dbPath = _config.dbPath.empty() ?
        PathUtils::getDefaultCursorDbPath() : PathUtils::expandHome(_config.dbPath);

    std::string tier;
    return extractTokenFromDb(dbPath, tier);
}

CursorStatus CursorCollector::fetchStatus() {
    std::lock_guard<std::mutex> lock(_mutex);
    CursorStatus status;
    std::string tier;
    std::string dbPath = _config.dbPath.empty() ?
        PathUtils::getDefaultCursorDbPath() : PathUtils::expandHome(_config.dbPath);

    std::string token = resolveAccessToken();
    if (token.empty()) {
        _hasLastValidStatus = false;
        _lastValidStatus = CursorStatus{};
        // Check if we at least found membership tier in DB
        extractTokenFromDb(dbPath, tier);
        if (!tier.empty()) {
            status.planTier = tier;
        }
        status.isAuthenticated = false;
        status.errorMessage = "Cursor access token not found (set CURSOR_ACCESS_TOKEN or log in to Cursor)";
        return status;
    }

    try {
        std::vector<std::pair<std::string, std::string>> headers = {
            {"Authorization", "Bearer " + token},
            {"Content-Type", "application/json"}
        };

        // 1. Primary modern endpoint: /auth/usage-summary (retry once on transient failure)
        CursorHttpResponse res;
        for (int attempt = 0; attempt < 2; ++attempt) {
            res = performRequest("GET", "/auth/usage-summary", headers);
            if (!isTransientHttpFailure(res)) {
                break;
            }
            if (!_httpRequestFn && attempt == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
            }
        }

        // Only fall back to legacy endpoints when /auth/usage-summary is unsupported (e.g. 404/405),
        // not when the server is experiencing a transient connection/5xx error or auth rejection.
        if (res.connected && (res.status == 404 || res.status == 405)) {
            // 2. Fallback endpoint: /aiserver.v1.DashboardService/GetCurrentPeriodUsage
            res = performRequest("POST", "/aiserver.v1.DashboardService/GetCurrentPeriodUsage", headers, "{}");
            if (!res.connected || res.status != 200) {
                // 3. Fallback endpoint: /auth/usage
                res = performRequest("GET", "/auth/usage", headers);
            }
        }

        if (!res.connected || res.status != 200) {
            if (isTransientHttpFailure(res) && _hasLastValidStatus) {
                return _lastValidStatus;
            }
            if (res.connected && (res.status == 401 || res.status == 403)) {
                _hasLastValidStatus = false;
                _lastValidStatus = CursorStatus{};
            }
            status.isAuthenticated = false;
            status.errorMessage = "Cursor API request failed (status: " +
                (res.connected ? std::to_string(res.status) : "connection error") + ")";
            return status;
        }

        nlohmann::json j = nlohmann::json::parse(res.body, nullptr, false);
        if (j.is_discarded()) {
            if (_hasLastValidStatus) {
                return _lastValidStatus;
            }
            status.isAuthenticated = false;
            status.errorMessage = "Invalid JSON response received from api2.cursor.sh";
            return status;
        }

        status.isAuthenticated = true;

        if (j.contains("membershipType") && j["membershipType"].is_string()) {
            status.planTier = j["membershipType"].get<std::string>();
        } else if (j.contains("plan") && j["plan"].is_string()) {
            status.planTier = j["plan"].get<std::string>();
        }

        std::string cycleStartIso;
        if (j.contains("billingCycleStart") && j["billingCycleStart"].is_string()) {
            cycleStartIso = j["billingCycleStart"].get<std::string>();
        }

        if (j.contains("billingCycleEnd") && j["billingCycleEnd"].is_string()) {
            status.cycleResetIso = j["billingCycleEnd"].get<std::string>();
        } else if (j.contains("periodEnd") && j["periodEnd"].is_string()) {
            status.cycleResetIso = j["periodEnd"].get<std::string>();
        } else if (j.contains("startOfMonth") && j["startOfMonth"].is_string()) {
            status.cycleResetIso = j["startOfMonth"].get<std::string>();
        }

        // Modern individualUsage.overall (Enterprise and Pro accounts)
        if (j.contains("individualUsage") && j["individualUsage"].is_object()) {
            const auto& ind = j["individualUsage"];
            if (ind.contains("overall") && ind["overall"].is_object()) {
                const auto& ov = ind["overall"];
                if (ov.contains("used") && ov["used"].is_number()) {
                    status.fastRequestsUsed = ov["used"].get<int>();
                }
                if (ov.contains("limit") && ov["limit"].is_number()) {
                    status.fastRequestsLimit = ov["limit"].get<int>();
                }
            } else if (ind.contains("fast") && ind["fast"].is_object()) {
                const auto& fast = ind["fast"];
                if (fast.contains("used") && fast["used"].is_number()) {
                    status.fastRequestsUsed = fast["used"].get<int>();
                }
                if (fast.contains("limit") && fast["limit"].is_number()) {
                    status.fastRequestsLimit = fast["limit"].get<int>();
                }
            }
        }

        // Fallback for older /auth/usage or /GetCurrentPeriodUsage format
        if (status.fastRequestsLimit == 0) {
            if (j.contains("numRequests") && j["numRequests"].is_number()) {
                status.fastRequestsUsed = j["numRequests"].get<int>();
            }
            if (j.contains("maxRequestUsage") && j["maxRequestUsage"].is_number()) {
                status.fastRequestsLimit = j["maxRequestUsage"].get<int>();
            }
            if (j.contains("gpt-4") && j["gpt-4"].is_object()) {
                const auto& g4 = j["gpt-4"];
                if (status.fastRequestsUsed == 0 && g4.contains("numRequests") && g4["numRequests"].is_number()) {
                    status.fastRequestsUsed = g4["numRequests"].get<int>();
                }
                if (status.fastRequestsLimit == 0 && g4.contains("maxRequestUsage") && g4["maxRequestUsage"].is_number()) {
                    status.fastRequestsLimit = g4["maxRequestUsage"].get<int>();
                }
            }
        }

        // On-demand team spend if present
        if (j.contains("teamUsage") && j["teamUsage"].is_object()) {
            const auto& team = j["teamUsage"];
            if (team.contains("onDemand") && team["onDemand"].is_object()) {
                const auto& od = team["onDemand"];
                if (od.contains("used") && od["used"].is_number()) {
                    status.onDemandSpend = od["used"].get<double>() / 100.0;
                }
            }
        }

        // Fetch personal usage and cumulative spend history
        fetchSpendData(token, cycleStartIso, status.cycleResetIso, status);

        _lastValidStatus = status;
        _hasLastValidStatus = true;

    } catch (const std::exception& e) {
        if (_hasLastValidStatus) {
            return _lastValidStatus;
        }
        status.isAuthenticated = false;
        status.errorMessage = std::string("Exception querying Cursor API: ") + e.what();
    }

    return status;
}

void CursorCollector::fetchSpendData(const std::string& token,
                                     const std::string& cycleStartIso,
                                     const std::string& cycleEndIso,
                                     CursorStatus& status) {
    int64_t startMs = parseIsoToEpochMs(cycleStartIso);
    int64_t endMs = parseIsoToEpochMs(cycleEndIso);

    // Fallback to beginning of current month and end of current month if missing
    if (startMs <= 0 || endMs <= 0) {
        auto now = std::chrono::system_clock::now();
        time_t tNow = std::chrono::system_clock::to_time_t(now);
        std::tm tmNow = {};
        gmtime_r(&tNow, &tmNow);

        std::tm tmStart = tmNow;
        tmStart.tm_mday = 1;
        tmStart.tm_hour = 0;
        tmStart.tm_min = 0;
        tmStart.tm_sec = 0;
        startMs = static_cast<int64_t>(timegm(&tmStart)) * 1000;

        std::tm tmEnd = tmStart;
        if (tmEnd.tm_mon == 11) {
            tmEnd.tm_year += 1;
            tmEnd.tm_mon = 0;
        } else {
            tmEnd.tm_mon += 1;
        }
        endMs = static_cast<int64_t>(timegm(&tmEnd)) * 1000;
    }

    std::vector<std::pair<std::string, std::string>> headers = {
        {"Authorization", "Bearer " + token},
        {"Cookie", "WorkosCursorSessionToken=" + token},
        {"Connect-Protocol-Version", "1"},
        {"Content-Type", "application/json"}
    };

    // 1. Current billing cycle query
    nlohmann::json reqBody = {
        {"periodStartMs", std::to_string(startMs)},
        {"periodEndMs", std::to_string(endMs)}
    };

    bool currentSpendTransientFailure = false;
    try {
        CursorHttpResponse res;
        for (int attempt = 0; attempt < 2; ++attempt) {
            res = performRequest("POST", "/aiserver.v1.DashboardService/GetDailySpendByCategory",
                                 headers, reqBody.dump());
            if (!isTransientHttpFailure(res)) {
                break;
            }
            if (!_httpRequestFn && attempt == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
            }
        }
        currentSpendTransientFailure = isTransientHttpFailure(res);

        if (res.connected && res.status == 200) {
            nlohmann::json spendJson = nlohmann::json::parse(res.body, nullptr, false);
            if (!spendJson.is_discarded() && spendJson.contains("dailySpend") && spendJson["dailySpend"].is_array()) {
                std::map<int64_t, double> dailyTotals;
                std::map<std::string, double> categoryTotals;
                double totalCents = 0.0;
                status.dailyCategorySpends.clear();
                std::string cycleIdStr = formatEpochMsToDate(startMs);

                for (const auto& item : spendJson["dailySpend"]) {
                    if (!item.is_object()) continue;
                    int64_t dayMs = 0;
                    if (item.contains("day")) {
                        if (item["day"].is_string()) {
                            try { dayMs = std::stoll(item["day"].get<std::string>()); } catch (...) {}
                        } else if (item["day"].is_number()) {
                            dayMs = item["day"].get<int64_t>();
                        }
                    }

                    double spendCents = 0.0;
                    if (item.contains("spendCents") && item["spendCents"].is_number()) {
                        spendCents = item["spendCents"].get<double>();
                    }

                    std::string cat = "other";
                    if (item.contains("category") && item["category"].is_string()) {
                        cat = item["category"].get<std::string>();
                    }

                    dailyTotals[dayMs] += spendCents;
                    categoryTotals[cat] += spendCents;
                    totalCents += spendCents;

                    if (dayMs > 0) {
                        CursorDailyCategorySpend cdcs;
                        cdcs.dayMs = dayMs;
                        cdcs.dayStr = formatEpochMsToDate(dayMs);
                        cdcs.category = cat;
                        cdcs.spendCents = spendCents;
                        cdcs.cycleId = cycleIdStr;
                        status.dailyCategorySpends.push_back(cdcs);
                    }
                }

                status.totalSpendUsd = totalCents / 100.0;

                double runningCumUsd = 0.0;
                status.dailySpendHistory.clear();
                for (const auto& kv : dailyTotals) {
                    DailySpendPoint pt;
                    pt.dayMs = kv.first;
                    pt.dayStr = formatEpochMsToDate(kv.first);
                    pt.spendUsd = kv.second / 100.0;
                    runningCumUsd += pt.spendUsd;
                    pt.cumulativeUsd = runningCumUsd;
                    status.dailySpendHistory.push_back(pt);
                }

                status.categorySpendList.clear();
                for (const auto& kv : categoryTotals) {
                    CategorySpend cs;
                    cs.category = kv.first;
                    cs.spendUsd = kv.second / 100.0;
                    cs.percentage = (totalCents > 0.0) ? (kv.second / totalCents) * 100.0 : 0.0;
                    status.categorySpendList.push_back(cs);
                }

                std::sort(status.categorySpendList.begin(), status.categorySpendList.end(),
                          [](const CategorySpend& a, const CategorySpend& b) {
                              return a.spendUsd > b.spendUsd;
                          });
            }
        }
    } catch (...) {
        currentSpendTransientFailure = true;
    }

    if (status.dailySpendHistory.empty() && currentSpendTransientFailure &&
        _hasLastValidStatus && _lastValidStatus.usageMode == "spend" &&
        !_lastValidStatus.dailySpendHistory.empty()) {
        status.totalSpendUsd = _lastValidStatus.totalSpendUsd;
        status.dailySpendHistory = _lastValidStatus.dailySpendHistory;
        status.categorySpendList = _lastValidStatus.categorySpendList;
        status.dailyCategorySpends = _lastValidStatus.dailyCategorySpends;
        status.usageMode = _lastValidStatus.usageMode;
    } else if (status.dailySpendHistory.empty()) {
        if (status.fastRequestsLimit > 0 || status.fastRequestsUsed > 0) {
            status.usageMode = "requests";
            if (_historyStore) {
                time_t startSec = (startMs > 0) ? static_cast<time_t>(startMs / 1000) : 0;
                time_t endSec = (endMs > 0) ? static_cast<time_t>(endMs / 1000) : std::numeric_limits<time_t>::max();
                status.dailySpendHistory = _historyStore->queryDailyUsageSummary("cursor", "fast_requests_used", startSec, endSec);
            }

            auto now = std::chrono::system_clock::now();
            time_t tNow = std::chrono::system_clock::to_time_t(now);
            std::tm tmNow = {};
            gmtime_r(&tNow, &tmNow);
            char todayBuf[32];
            std::snprintf(todayBuf, sizeof(todayBuf), "%04d-%02d-%02d", tmNow.tm_year + 1900, tmNow.tm_mon + 1, tmNow.tm_mday);
            std::string todayStr = todayBuf;

            std::tm tmDay = tmNow;
            tmDay.tm_hour = 0;
            tmDay.tm_min = 0;
            tmDay.tm_sec = 0;
            int64_t todayMs = static_cast<int64_t>(timegm(&tmDay)) * 1000;

            if (status.dailySpendHistory.empty()) {
                DailySpendPoint pt;
                pt.dayMs = todayMs;
                pt.dayStr = todayStr;
                pt.requestsUsed = status.fastRequestsUsed;
                pt.cumulativeRequests = status.fastRequestsUsed;
                status.dailySpendHistory.push_back(pt);
            } else {
                if (status.dailySpendHistory.back().dayStr == todayStr) {
                    if (status.fastRequestsUsed > status.dailySpendHistory.back().cumulativeRequests) {
                        int delta = status.fastRequestsUsed - status.dailySpendHistory.back().cumulativeRequests;
                        status.dailySpendHistory.back().requestsUsed += delta;
                        status.dailySpendHistory.back().cumulativeRequests = status.fastRequestsUsed;
                    }
                } else {
                    DailySpendPoint pt;
                    pt.dayMs = todayMs;
                    pt.dayStr = todayStr;
                    int prev = status.dailySpendHistory.back().cumulativeRequests;
                    pt.requestsUsed = (status.fastRequestsUsed >= prev) ? (status.fastRequestsUsed - prev) : status.fastRequestsUsed;
                    pt.cumulativeRequests = status.fastRequestsUsed;
                    status.dailySpendHistory.push_back(pt);
                }
            }
        }
    } else {
        status.usageMode = "spend";
    }

    // 2. Previous billing cycle query (for comparison against last month)
    int64_t durationMs = endMs - startMs;
    if (durationMs <= 0) durationMs = 30LL * 24 * 3600 * 1000;
    int64_t prevStartMs = startMs - durationMs;
    int64_t prevEndMs = startMs;

    nlohmann::json prevReqBody = {
        {"periodStartMs", std::to_string(prevStartMs)},
        {"periodEndMs", std::to_string(prevEndMs)}
    };

    try {
        CursorHttpResponse prevRes;
        for (int attempt = 0; attempt < 2; ++attempt) {
            prevRes = performRequest("POST", "/aiserver.v1.DashboardService/GetDailySpendByCategory",
                                     headers, prevReqBody.dump());
            if (!isTransientHttpFailure(prevRes)) {
                break;
            }
            if (!_httpRequestFn && attempt == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
            }
        }

        if (prevRes.connected && prevRes.status == 200) {
            nlohmann::json prevJson = nlohmann::json::parse(prevRes.body, nullptr, false);
            if (!prevJson.is_discarded() && prevJson.contains("dailySpend") && prevJson["dailySpend"].is_array()) {
                double prevTotalCents = 0.0;
                for (const auto& item : prevJson["dailySpend"]) {
                    if (item.is_object() && item.contains("spendCents") && item["spendCents"].is_number()) {
                        prevTotalCents += item["spendCents"].get<double>();
                    }
                }
                status.prevCycleSpendUsd = prevTotalCents / 100.0;
            }
        } else if (isTransientHttpFailure(prevRes) && _hasLastValidStatus && _lastValidStatus.prevCycleSpendUsd > 0.0) {
            status.prevCycleSpendUsd = _lastValidStatus.prevCycleSpendUsd;
        }
    } catch (...) {
        if (_hasLastValidStatus && _lastValidStatus.prevCycleSpendUsd > 0.0) {
            status.prevCycleSpendUsd = _lastValidStatus.prevCycleSpendUsd;
        }
    }
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
