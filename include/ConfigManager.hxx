/*
 * ConfigManager.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_CONFIG_MANAGER_HXX
#define AIMON_CONFIG_MANAGER_HXX

#include <string>
#include <vector>
#include <mutex>
#include <nlohmann/json.hpp>

namespace aimon {

struct PollingConfig {
    int baseIntervalSec = 300;
    int idleIntervalSec = 600;
    int activeIntervalSec = 120;
};

struct WebConfig {
    std::string host = "0.0.0.0";
    int port = 3883;
    bool endpointsEnabled = false;
};

struct MqttConfig {
    bool enabled = false;
    std::string broker = "localhost";
    int port = 1883;
    std::string username;
    std::string password;
    std::string topicPrefix = "aimon";
    std::string discoveryPrefix = "homeassistant";
    bool retain = true;
};

struct HistoryConfig {
    bool enabled = true;
    std::string dbPath;
};

struct AntigravityConfig {
    bool autoDiscover = true;
    int port = 0;
    std::string csrfToken;
    std::string defaultModel = "Antigravity";
};

struct CursorConfig {
    bool autoDiscover = true;
    std::string dbPath;
    std::string accessToken;
};

struct ClaudeAccountConfig {
    std::string name;
    std::string configDir;          // Claude Code configuration directory (holds projects/)
    double spendLimitUsd = 0.0;     // 0 = not configured
    int cycleResetDay = 0;          // 1..28 (UTC), 0 = not configured
};

struct ClaudeConfig {
    bool enabled = true;
    int windowDays = 30;
    int retentionDays = 400;
    std::string pricingUrl;         // empty = the official pricing page
    int pricingRefreshHours = 24;
    std::vector<ClaudeAccountConfig> accounts;

    // The configured accounts, or one "default" account at $CLAUDE_CONFIG_DIR
    // (else ~/.claude) when none are configured.
    std::vector<ClaudeAccountConfig> resolvedAccounts() const;
    bool validate(std::string& error) const;
};

struct GatewayConfig {
    bool enabled = true;
    std::string host = "0.0.0.0";
    int port = 3885;
    bool allowCliNewSessionSpawn = true;
    bool allowAgyAgentApiFallback = true;
    bool strictPresenceFdCheck = true;
    bool autoPruneStalePresenceLocks = true;
    bool pruneStalePresenceLocks = true;
    int presenceStaleTtlSec = 3600;
    int stalePresenceLockMaxAgeSec = 3600;
    int activeRecencyWindowSec = 120;
    int telemetryRetentionDays = 14;

    nlohmann::json toJson() const {
        return {
            {"enabled", enabled},
            {"host", host},
            {"port", port},
            {"allow_cli_new_session_spawn", allowCliNewSessionSpawn},
            {"allow_agy_agentapi_fallback", allowAgyAgentApiFallback},
            {"strict_presence_fd_check", strictPresenceFdCheck},
            {"auto_prune_stale_presence_locks", autoPruneStalePresenceLocks},
            {"prune_stale_presence_locks", autoPruneStalePresenceLocks},
            {"presence_stale_ttl_sec", presenceStaleTtlSec},
            {"stale_presence_lock_max_age_sec", presenceStaleTtlSec},
            {"active_recency_window_sec", activeRecencyWindowSec},
            {"telemetry_retention_days", telemetryRetentionDays}
        };
    }

    void fromJson(const nlohmann::json& j) {
        if (!j.is_object()) return;
        if (j.contains("enabled") && j["enabled"].is_boolean()) enabled = j["enabled"];
        if (j.contains("host") && j["host"].is_string()) host = j["host"];
        if (j.contains("port") && j["port"].is_number_integer()) port = j["port"];
        if (j.contains("allow_cli_new_session_spawn") && j["allow_cli_new_session_spawn"].is_boolean()) {
            allowCliNewSessionSpawn = j["allow_cli_new_session_spawn"];
        }
        if (j.contains("allow_agy_agentapi_fallback") && j["allow_agy_agentapi_fallback"].is_boolean()) {
            allowAgyAgentApiFallback = j["allow_agy_agentapi_fallback"];
        }
        if (j.contains("strict_presence_fd_check") && j["strict_presence_fd_check"].is_boolean()) {
            strictPresenceFdCheck = j["strict_presence_fd_check"];
        }
        if (j.contains("prune_stale_presence_locks") && j["prune_stale_presence_locks"].is_boolean()) {
            pruneStalePresenceLocks = j["prune_stale_presence_locks"];
            autoPruneStalePresenceLocks = pruneStalePresenceLocks;
        }
        if (j.contains("auto_prune_stale_presence_locks") && j["auto_prune_stale_presence_locks"].is_boolean()) {
            autoPruneStalePresenceLocks = j["auto_prune_stale_presence_locks"];
            pruneStalePresenceLocks = autoPruneStalePresenceLocks;
        }
        if (j.contains("stale_presence_lock_max_age_sec") && j["stale_presence_lock_max_age_sec"].is_number_integer()) {
            stalePresenceLockMaxAgeSec = j["stale_presence_lock_max_age_sec"];
            presenceStaleTtlSec = stalePresenceLockMaxAgeSec;
        }
        if (j.contains("presence_stale_ttl_sec") && j["presence_stale_ttl_sec"].is_number_integer()) {
            presenceStaleTtlSec = j["presence_stale_ttl_sec"];
            stalePresenceLockMaxAgeSec = presenceStaleTtlSec;
        }
        if (j.contains("active_recency_window_sec") && j["active_recency_window_sec"].is_number_integer()) {
            activeRecencyWindowSec = j["active_recency_window_sec"];
        }
        if (j.contains("telemetry_retention_days") && j["telemetry_retention_days"].is_number_integer()) {
            telemetryRetentionDays = j["telemetry_retention_days"];
        }
    }

    bool validate(std::string& error) const {
        if (port < 1 || port > 65535) {
            error = "gateway.port must be between 1 and 65535";
            return false;
        }
        if (presenceStaleTtlSec < 10 || presenceStaleTtlSec > 604800) {
            error = "gateway.presence_stale_ttl_sec must be between 10 and 604800";
            return false;
        }
        if (stalePresenceLockMaxAgeSec < 10 || stalePresenceLockMaxAgeSec > 604800) {
            error = "gateway.stale_presence_lock_max_age_sec must be between 10 and 604800";
            return false;
        }
        if (activeRecencyWindowSec < 10 || activeRecencyWindowSec > 3600) {
            error = "gateway.active_recency_window_sec must be between 10 and 3600";
            return false;
        }
        if (telemetryRetentionDays < 1 || telemetryRetentionDays > 365) {
            error = "gateway.telemetry_retention_days must be between 1 and 365";
            return false;
        }
        return true;
    }
};

struct InsightsConfig {
    bool enabled = true;
    std::string dbPath = "~/.config/aimon/insights.db";
    int retentionDays = 400;
    int refreshIntervalSec = 300;
    int longContextThresholdTokens = 150000;
    int compactContextThresholdTokens = 100000;
    int compactContinueRequests = 15;
    double subagentShareThreshold = 0.30;
    int longSessionHours = 8;
    double cacheEfficiencyThreshold = 0.50;
    int cacheEfficiencyMinRequests = 20;
    int errorRetryStreakThreshold = 3;
    int idleGapSeconds = 300;
    double burnRateAlertFraction = 0.75;
    int shortPromptMaxProseChars = 60;
    int dripPromptMaxProseChars = 80;
    int dripPromptsPerHour = 25;
    int quickRepromptSeconds = 120;
    int coachingMinSampleSize = 10;
};

struct EdaAcceleratorConfig {
    bool enabled = true;
    int saPlacementIterations = 50000;
    int autorouteMaxRipupPasses = 25;
    double autorouteGridMm = 0.25;
    bool preferJlcpcbBasicESeries = true;
    double defaultCopperWeightOz = 1.0;
    std::string defaultFilamentMaterial = "PETG";
    double defaultGearPressureAngleDeg = 20.0;
};

struct EasyEdaToolConfig {
    bool enabled = true;
};

struct KiCadToolConfig {
    bool enabled = true;
};

struct FreeCadToolConfig {
    bool enabled = true;
};

struct EdaBridgeConfig {
    bool enabled = false;
    std::string mode = "bidirectional";
    std::string host;
    int port = 3892;
    std::string authSecret;
    int healthPollIntervalSec = 30;
    int timeoutMs = 60000;
    int maxOutputBytes = 4194304;

    EdaAcceleratorConfig accelerators;
    EasyEdaToolConfig easyeda;
    KiCadToolConfig kicad;
    FreeCadToolConfig freecad;

    nlohmann::json toJson(bool redactSecret = false) const;
    void fromJson(const nlohmann::json& j);
    bool validate(std::string& error) const;
};

struct AimonConfig {
    PollingConfig polling;
    WebConfig web;
    MqttConfig mqtt;
    HistoryConfig history;
    AntigravityConfig antigravity;
    CursorConfig cursor;
    ClaudeConfig claude;
    GatewayConfig gateway;
    InsightsConfig insights;
    EdaBridgeConfig eda;

    nlohmann::json toJson() const;
    void fromJson(const nlohmann::json& j);
    bool validate(std::string& error) const;
};

class ConfigManager {
public:
    static ConfigManager& getInstance();
    ConfigManager();

    bool load(const std::string& customPath = "");
    bool save(const std::string& customPath = "") const;

    bool loadLibConfig(const std::string& customPath = "");
    bool saveLibConfig(const std::string& customPath = "") const;

    const AimonConfig& getConfig() const {
        return _config;
    }

    AimonConfig& getConfig() {
        return _config;
    }

    AimonConfig getConfigSnapshot() const;
    bool updateFromJson(const nlohmann::json& patch, std::string& error);

    const std::string& getLastErrorMessage() const {
        return _lastErrorMessage;
    }

    void applyEnvironmentOverrides();

private:
    mutable std::mutex _mutex;
    AimonConfig _config;
    std::string _configFilePath;
    std::string _lastErrorMessage;
};

} // namespace aimon

#endif // AIMON_CONFIG_MANAGER_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
