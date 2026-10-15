/*
 * ConfigManager.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_CONFIG_MANAGER_HXX
#define AIMON_CONFIG_MANAGER_HXX

#include <string>
#include <vector>
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
    std::string host;
    int port = 0;
    int pluginPort = 0;
    std::string executionMode = "auto";
    std::string customBinaryPath;
    std::string projectsDir = "~/work/pcb/easyeda";
    bool preferJlcpcbBasicParts = true;
    int minLcscStock = 100;
    bool autoDrcAfterEdit = true;
    int jlcpcbLayerCount = 2;
    int timeoutMs = 60000;
};

struct KiCadToolConfig {
    bool enabled = true;
    std::string host;
    int port = 0;
    int pluginPort = 0;
    std::string executionMode = "auto";
    std::string customBinaryPath;
    std::string symbolLibDir;
    std::string footprintLibDir;
    std::string projectsDir = "~/work/pcb/kicad";
    int schematicGridMils = 50;
    bool autoErcAfterEdit = true;
    bool drcSchematicParity = true;
    std::string fabTargetHouse = "jlcpcb";
    int timeoutMs = 60000;
};

struct FreeCadToolConfig {
    bool enabled = true;
    std::string host;
    int port = 0;
    int pluginPort = 0;
    std::string executionMode = "auto";
    std::string customBinaryPath;
    std::string projectsDir = "~/work/robot/cad";
    bool enforceTnpGuard = true;
    double defaultPrintToleranceMm = 0.20;
    double minWallThicknessMm = 1.20;
    double maxOverhangDeg = 45.0;
    double meshLinearDeflectionMm = 0.05;
    double meshAngularDeflectionDeg = 5.0;
    double jointSweepStepDeg = 5.0;
    int renderWidth = 1280;
    int renderHeight = 960;
    int timeoutMs = 120000;
};

struct EdaBridgeConfig {
    bool enabled = false;
    std::string mode = "bidirectional";
    std::string host;
    int port = 3892;
    int pluginPort = 3893;
    std::string authSecret;
    int healthPollIntervalSec = 30;
    int timeoutMs = 60000;
    int maxOutputBytes = 4194304;
    bool requireApprovalForExports = false;

    std::string linuxPathPrefix = "~/work";
    std::string windowsPathPrefix = "Z:\\";

    EdaAcceleratorConfig accelerators;
    EasyEdaToolConfig easyeda;
    KiCadToolConfig kicad;
    FreeCadToolConfig freecad;

    std::string resolvedHostFor(const std::string& toolHost) const {
        return toolHost.empty() ? host : toolHost;
    }

    int resolvedPortFor(int toolPort) const {
        return toolPort > 0 ? toolPort : port;
    }

    int resolvedPluginPortFor(int toolPluginPort) const {
        return toolPluginPort > 0 ? toolPluginPort : pluginPort;
    }

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
};

class ConfigManager {
public:
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

    const std::string& getLastErrorMessage() const {
        return _lastErrorMessage;
    }

    void applyEnvironmentOverrides();

private:
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
