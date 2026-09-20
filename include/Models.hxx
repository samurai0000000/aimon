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
        return {
            {"model_name", modelName},
            {"model_id", modelId},
            {"remaining_fraction", remainingFraction},
            {"reset_time_iso", resetTimeIso}
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

    nlohmann::json toJson() const {
        return {
            {"bucket_id", bucketId},
            {"display_name", displayName},
            {"window", window},
            {"description", description},
            {"remaining_fraction", remainingFraction},
            {"reset_time_iso", resetTimeIso}
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

    nlohmann::json toJson() const {
        return {
            {"day_ms", dayMs},
            {"day_str", dayStr},
            {"spend_usd", spendUsd},
            {"cumulative_usd", cumulativeUsd}
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

struct AggregateStatus {
    std::chrono::system_clock::time_point lastUpdated;
    AntigravityStatus antigravity;
    CursorStatus cursor;

    nlohmann::json toJson() const {
        auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
            lastUpdated.time_since_epoch()).count();

        return {
            {"last_updated_epoch", epoch},
            {"antigravity", antigravity.toJson()},
            {"cursor", cursor.toJson()}
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
