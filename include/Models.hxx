/*
 * Models.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_MODELS_HXX
#define AIMON_MODELS_HXX

#include <string>
#include <vector>
#include <chrono>
#include <nlohmann/json.hpp>

namespace aimon {

struct ModelQuota {
    std::string modelName;
    std::string modelId;
    double remainingFraction = 1.0;
    std::string resetTimeIso;
    std::chrono::system_clock::time_point resetTimestamp;

    nlohmann::json toJson() const {
        int64_t remainingSec = 0;
        auto now = std::chrono::system_clock::now();
        if (resetTimestamp > now) {
            remainingSec = std::chrono::duration_cast<std::chrono::seconds>(resetTimestamp - now).count();
        }
        return {
            {"model_name", modelName},
            {"model_id", modelId},
            {"remaining_fraction", remainingFraction},
            {"reset_time_iso", resetTimeIso},
            {"reset_time_remaining_seconds", remainingSec}
        };
    }
};

struct QuotaBucket {
    std::string bucketId;
    std::string displayName;
    std::string window;
    std::string description;
    double remainingFraction = 1.0;
    std::string resetTimeIso;
    std::chrono::system_clock::time_point resetTimestamp;

    nlohmann::json toJson() const {
        int64_t remainingSec = 0;
        auto now = std::chrono::system_clock::now();
        if (resetTimestamp > now) {
            remainingSec = std::chrono::duration_cast<std::chrono::seconds>(resetTimestamp - now).count();
        }
        return {
            {"bucket_id", bucketId},
            {"display_name", displayName},
            {"window", window},
            {"description", description},
            {"remaining_fraction", remainingFraction},
            {"reset_time_iso", resetTimeIso},
            {"reset_time_remaining_seconds", remainingSec}
        };
    }
};

struct QuotaGroup {
    std::string displayName;
    std::string description;
    std::vector<QuotaBucket> buckets;

    nlohmann::json toJson() const {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& b : buckets) {
            arr.push_back(b.toJson());
        }
        return {
            {"display_name", displayName},
            {"description", description},
            {"buckets", arr}
        };
    }
};

struct UserCredit {
    std::string creditType;
    int creditAmount = 0;
    int minimumCreditAmountForUsage = 0;

    nlohmann::json toJson() const {
        return {
            {"credit_type", creditType},
            {"credit_amount", creditAmount},
            {"minimum_credit_amount_for_usage", minimumCreditAmountForUsage}
        };
    }
};

struct AntigravityStatus {
    bool isRunning = false;
    std::string planTier = "Unknown";
    std::vector<ModelQuota> models;
    std::vector<QuotaGroup> quotaGroups;
    std::vector<UserCredit> availableCredits;
    int availablePromptCredits = 0;
    int availableFlowCredits = 0;
    int monthlyPromptCredits = 0;
    int monthlyFlowCredits = 0;
    std::string errorMessage;

    nlohmann::json toJson() const {
        nlohmann::json modelsArray = nlohmann::json::array();
        for (const auto& m : models) {
            modelsArray.push_back(m.toJson());
        }

        nlohmann::json groupsArray = nlohmann::json::array();
        for (const auto& g : quotaGroups) {
            groupsArray.push_back(g.toJson());
        }

        nlohmann::json creditsArray = nlohmann::json::array();
        for (const auto& c : availableCredits) {
            creditsArray.push_back(c.toJson());
        }

        return {
            {"is_running", isRunning},
            {"plan_tier", planTier},
            {"models", modelsArray},
            {"quota_groups", groupsArray},
            {"available_credits", creditsArray},
            {"available_prompt_credits", availablePromptCredits},
            {"available_flow_credits", availableFlowCredits},
            {"monthly_prompt_credits", monthlyPromptCredits},
            {"monthly_flow_credits", monthlyFlowCredits},
            {"error_message", errorMessage}
        };
    }
};

struct DailySpendPoint {
    int64_t dayMs = 0;
    std::string dayStr;
    double spendUsd = 0.0;
    double cumulativeUsd = 0.0;
    int requestsUsed = 0;
    int cumulativeRequests = 0;

    nlohmann::json toJson() const {
        return {
            {"day_ms", dayMs},
            {"day_str", dayStr},
            {"spend_usd", spendUsd},
            {"cumulative_usd", cumulativeUsd},
            {"requests_used", requestsUsed},
            {"cumulative_requests", cumulativeRequests}
        };
    }
};

struct CategorySpend {
    std::string category;
    double spendUsd = 0.0;
    double percentage = 0.0;

    nlohmann::json toJson() const {
        return {
            {"category", category},
            {"spend_usd", spendUsd},
            {"percentage", percentage}
        };
    }
};

struct CursorStatus {
    bool isAuthenticated = false;
    std::string planTier = "Unknown";
    std::string usageMode = "spend";
    int fastRequestsUsed = 0;
    int fastRequestsLimit = 0;
    double onDemandSpend = 0.0;
    double totalSpendUsd = 0.0;
    double prevCycleSpendUsd = 0.0;
    std::vector<DailySpendPoint> dailySpendHistory;
    std::vector<CategorySpend> categorySpendList;
    std::string cycleResetIso;
    std::string errorMessage;

    nlohmann::json toJson() const {
        nlohmann::json dailyArr = nlohmann::json::array();
        for (const auto& d : dailySpendHistory) {
            dailyArr.push_back(d.toJson());
        }

        nlohmann::json catArr = nlohmann::json::array();
        for (const auto& c : categorySpendList) {
            catArr.push_back(c.toJson());
        }

        return {
            {"is_authenticated", isAuthenticated},
            {"plan_tier", planTier},
            {"usage_mode", usageMode},
            {"fast_requests_used", fastRequestsUsed},
            {"fast_requests_limit", fastRequestsLimit},
            {"on_demand_spend", onDemandSpend},
            {"total_spend_usd", totalSpendUsd},
            {"prev_cycle_spend_usd", prevCycleSpendUsd},
            {"daily_spend", dailyArr},
            {"spend_by_category", catArr},
            {"cycle_reset_iso", cycleResetIso},
            {"error_message", errorMessage}
        };
    }
};

enum class ClaudeTier { Unknown, Enterprise, Personal };

inline const char* claudeTierName(ClaudeTier tier) {
    switch (tier) {
    case ClaudeTier::Enterprise: return "enterprise";
    case ClaudeTier::Personal: return "personal";
    default: return "unknown";
    }
}

inline double claudeNanoToUsd(int64_t nano) {
    return static_cast<double>(nano) / 1e9;
}

struct ClaudeModelUsage {
    std::string model;
    int64_t messages = 0;
    int64_t input = 0;
    int64_t output = 0;
    int64_t cacheRead = 0;
    int64_t cacheWrite5m = 0;
    int64_t cacheWrite1h = 0;
    int64_t costNano = 0;
    int64_t unpricedMessages = 0;

    nlohmann::json toJson() const {
        return {
            {"model", model},
            {"messages", messages},
            {"input_tokens", input},
            {"output_tokens", output},
            {"cache_read_tokens", cacheRead},
            {"cache_write_5m_tokens", cacheWrite5m},
            {"cache_write_1h_tokens", cacheWrite1h},
            {"est_cost_nano", costNano},
            {"est_cost_usd", claudeNanoToUsd(costNano)},
            {"unpriced_messages", unpricedMessages}
        };
    }
};

struct ClaudeWindowTotals {
    std::string label;
    int64_t fromEpoch = 0;
    int64_t toEpoch = 0;
    int64_t messages = 0;
    int64_t input = 0;
    int64_t output = 0;
    int64_t cacheRead = 0;
    int64_t cacheWrite5m = 0;
    int64_t cacheWrite1h = 0;
    int64_t costNano = 0;
    int64_t unpricedMessages = 0;
    bool costOverflow = false;

    int64_t totalTokens() const {
        return input + output + cacheRead + cacheWrite5m + cacheWrite1h;
    }

    nlohmann::json toJson() const {
        return {
            {"label", label},
            {"from_epoch", fromEpoch},
            {"to_epoch", toEpoch},
            {"messages", messages},
            {"input_tokens", input},
            {"output_tokens", output},
            {"cache_read_tokens", cacheRead},
            {"cache_write_5m_tokens", cacheWrite5m},
            {"cache_write_1h_tokens", cacheWrite1h},
            {"total_tokens", totalTokens()},
            {"est_cost_nano", costNano},
            {"est_cost_usd", claudeNanoToUsd(costNano)},
            {"unpriced_messages", unpricedMessages},
            {"cost_overflow", costOverflow}
        };
    }
};

struct ClaudeDailyPoint {
    std::string dayStr;
    int64_t dayMs = 0;
    int64_t messages = 0;
    int64_t tokens = 0;
    int64_t costNano = 0;

    nlohmann::json toJson() const {
        return {
            {"day_str", dayStr},
            {"day_ms", dayMs},
            {"messages", messages},
            {"tokens", tokens},
            {"est_cost_nano", costNano},
            {"est_cost_usd", claudeNanoToUsd(costNano)}
        };
    }
};

struct ClaudePricingStatus {
    std::string sourceUrl;
    int64_t fetchedAtEpoch = 0;
    bool stale = true;
    size_t modelsLoaded = 0;
    size_t quarantinedCount = 0;
    size_t versionCount = 0;
    std::string error;

    nlohmann::json toJson() const {
        return {
            {"source_url", sourceUrl},
            {"fetched_at_epoch", fetchedAtEpoch},
            {"stale", stale},
            {"models_loaded", modelsLoaded},
            {"quarantined_count", quarantinedCount},
            {"version_count", versionCount},
            {"error", error}
        };
    }
};

struct ClaudeAccountStatus {
    std::string name;
    ClaudeTier tier = ClaudeTier::Unknown;
    std::string rawSubscriptionType;
    std::string rawRateLimitTier;
    bool hasData = false;
    std::string errorMessage;
    std::string warning;
    int windowDays = 30;
    ClaudeWindowTotals last5h;
    ClaudeWindowTotals last7d;
    ClaudeWindowTotals window;
    ClaudeWindowTotals today;
    bool cycleConfigured = false;
    ClaudeWindowTotals cycle;
    double spendLimitUsd = 0.0;
    double estPctOfLimit = 0.0;
    std::string cycleResetIso;
    std::vector<ClaudeModelUsage> models;
    std::vector<ClaudeDailyPoint> daily;
    std::vector<std::string> unpricedModels;
    int64_t skippedLines = 0;
    int64_t filesScanned = 0;
    int64_t bytesReadLastPoll = 0;
    int64_t rowsInStore = 0;

    nlohmann::json toJson() const {
        nlohmann::json modelsArr = nlohmann::json::array();
        for (const auto& m : models) {
            modelsArr.push_back(m.toJson());
        }
        nlohmann::json dailyArr = nlohmann::json::array();
        for (const auto& d : daily) {
            dailyArr.push_back(d.toJson());
        }
        return {
            {"name", name},
            {"tier", claudeTierName(tier)},
            {"raw_subscription_type", rawSubscriptionType},
            {"raw_rate_limit_tier", rawRateLimitTier},
            {"has_data", hasData},
            {"error_message", errorMessage},
            {"warning", warning},
            {"window_days", windowDays},
            {"last_5h", last5h.toJson()},
            {"last_7d", last7d.toJson()},
            {"window", window.toJson()},
            {"today", today.toJson()},
            {"cycle_configured", cycleConfigured},
            {"cycle", cycle.toJson()},
            {"spend_limit_usd", spendLimitUsd},
            {"est_pct_of_limit", estPctOfLimit},
            {"cycle_reset_iso", cycleResetIso},
            {"models", modelsArr},
            {"daily", dailyArr},
            {"unpriced_models", unpricedModels},
            {"skipped_lines", skippedLines},
            {"files_scanned", filesScanned},
            {"bytes_read_last_poll", bytesReadLastPoll},
            {"rows_in_store", rowsInStore}
        };
    }
};

struct ClaudeStatus {
    bool enabled = false;
    ClaudePricingStatus pricing;
    std::vector<ClaudeAccountStatus> accounts;

    nlohmann::json toJson() const {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& a : accounts) {
            arr.push_back(a.toJson());
        }
        return {
            {"enabled", enabled},
            {"pricing", pricing.toJson()},
            {"accounts", arr}
        };
    }
};

struct AggregateStatus {
    std::chrono::system_clock::time_point lastUpdated;
    AntigravityStatus antigravity;
    CursorStatus cursor;
    ClaudeStatus claude;

    nlohmann::json toJson() const {
        auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
            lastUpdated.time_since_epoch()).count();
        auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        return {
            {"server_timestamp_ms", nowMs},
            {"last_updated_epoch", epoch},
            {"antigravity", antigravity.toJson()},
            {"cursor", cursor.toJson()},
            {"claude", claude.toJson()}
        };
    }
};



struct ClientSession {
    std::string sessionId;
    std::string clientName = "MCP Client";
    std::string clientVersion;
    std::string remoteIp;
    int64_t connectedTimeEpoch = 0;
    int64_t lastHeartbeatEpoch = 0;
    bool active = true;

    nlohmann::json toJson() const {
        return {
            {"session_id", sessionId},
            {"client_name", clientName},
            {"client_version", clientVersion},
            {"remote_ip", remoteIp},
            {"connected_time_epoch", connectedTimeEpoch},
            {"last_heartbeat_epoch", lastHeartbeatEpoch},
            {"active", active}
        };
    }
};

struct DiscoveredMonitor {
    std::string id;
    std::string name;
    std::string shortName;
    std::string subsystem;
    std::string host;
    int port = 0;
    std::string path = "/";
    bool connected = false;
    bool reachable = false;
    bool isSelf = false;
    int priority = 100;
    int64_t lastSeenEpoch = 0;

    nlohmann::json toJson() const {
        return {
            {"id", id},
            {"name", name},
            {"short_name", shortName},
            {"subsystem", subsystem},
            {"host", host},
            {"port", port},
            {"path", path},
            {"connected", connected},
            {"reachable", reachable},
            {"is_self", isSelf},
            {"isSelf", isSelf},
            {"priority", priority},
            {"last_seen_epoch", lastSeenEpoch}
        };
    }
};

} // namespace aimon

#endif // AIMON_MODELS_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
