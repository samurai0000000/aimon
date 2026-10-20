/*
 * EdaBridgeAppConfig.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_EDA_BRIDGE_APP_CONFIG_HXX
#define AIMON_EDA_BRIDGE_APP_CONFIG_HXX

#include <cstdint>
#include <string>
#include <vector>
#include "EdaRpcServer.hxx"
#include "Version.hxx"

namespace aimon {

struct EdaBridgeAppSettings {
    std::string version = AIMON_VERSION_STRING;
    bool enableEasyEda = true;
    bool enableKiCad = true;
    bool enableFreeCad = true;
    bool autoDeployPlugins = true;
    bool startMinimizedToTray = true;
    bool launchAtLogin = false;
    bool disallowExports = false;
    std::string connectionMode = "bidirectional"; // "listen" | "gateway" | "bidirectional"
    std::string listenHost = "0.0.0.0";
    int rpcPort = 3892;
    int pluginPort = 3893;
    std::string aimonHost = "127.0.0.1";
    int aimonWebPort = 3883;
    int gatewayPort = 3885;
    std::string authSecret;
    std::string allowedRoot = "";
    std::string kicadBinaryPath;
    std::string freecadBinaryPath;
    std::string kicadPluginDir;
    std::string freecadModDir;
};

struct AimonModelQuotaItem {
    std::string name;
    double remainingPct = 100.0;
    int64_t resetSeconds = 0;
    std::string resetTimeIso;
};

struct AimonQuotaBucketItem {
    std::string bucketId;
    std::string displayName;
    std::string window;
    std::string description;
    double remainingPct = 100.0;
    int64_t resetSeconds = 0;
    std::string resetTimeIso;
};

struct AimonQuotaGroupItem {
    std::string displayName;
    std::string description;
    std::vector<AimonQuotaBucketItem> buckets;
};

struct AimonClaudeAccountSummary {
    std::string name;
    std::string tier;
    int64_t tokens5h = 0;
    double cost5hUsd = 0.0;
    int64_t tokens7d = 0;
    double cost7dUsd = 0.0;
    int64_t tokensWindow = 0;
    double costWindowUsd = 0.0;
    bool cycleConfigured = false;
    double costCycleUsd = 0.0;
    double spendLimitUsd = 0.0;
    double estPctOfLimit = 0.0;
};

struct AimonQuotaSnapshot {
    bool reachable = false;
    std::string errorMessage;
    int64_t lastUpdatedEpoch = 0;

    // Antigravity
    bool agRunning = false;
    std::string agPlanTier = "Unknown";
    bool agHasGoogleOneCredits = false;
    int agGoogleOneCredits = 0;
    int agMinCreditPerReq = 0;
    int agPromptCredits = 0;
    int agFlowCredits = 0;
    int agMonthlyPromptCredits = 0;
    int agMonthlyFlowCredits = 0;
    std::vector<AimonQuotaGroupItem> agQuotaGroups;
    std::vector<AimonModelQuotaItem> agModels;
    std::string agError;

    // Cursor
    bool cursorAuthenticated = false;
    std::string cursorPlanTier = "Unknown";
    std::string cursorUsageMode = "spend";
    int cursorRequestsUsed = 0;
    int cursorRequestsLimit = 0;
    double cursorTotalSpendUsd = 0.0;
    double cursorOnDemandSpendUsd = 0.0;
    std::string cursorCycleResetIso;
    std::string cursorError;

    // Claude Code
    bool claudeEnabled = false;
    std::vector<AimonClaudeAccountSummary> claudeAccounts;

    // EDA / CAD Bridge Telemetry (from aimon /api/status root["eda"])
    std::vector<EdaBridgeComponentStat> edaComponents;
    std::vector<EdaBridgeCallLogEntry> edaRecentCalls;
};

class EdaBridgeAppConfig {
public:
    static std::string defaultConfigFilePath();
    static bool loadFromFile(const std::string& path,
                             EdaBridgeAppSettings& outSettings,
                             std::string& outError);
    static bool saveToFile(const std::string& path,
                           const EdaBridgeAppSettings& settings,
                           std::string& outError);
    static EdaRpcServerOptions toServerOptions(const EdaBridgeAppSettings& settings);

    // Embedded zero-Python plugin deployment & .eext ZIP packager
    static std::vector<uint8_t> buildEasyEdaEextZipBytes();
    static bool writeEasyEdaEextFile(const std::string& outputFilePath,
                                     std::string& outError);
    static bool deployKiCadPlugin(const std::string& appDataDir,
                                  std::string& outDeployedDir,
                                  std::string& outError,
                                  const std::string& customTargetDir = "",
                                  bool forceOverwrite = false,
                                  bool* outAlreadyDeployed = nullptr);
    static bool deployFreeCadPlugin(const std::string& appDataDir,
                                    std::string& outDeployedDir,
                                    std::string& outError,
                                    const std::string& customTargetDir = "",
                                    bool forceOverwrite = false,
                                    bool* outAlreadyDeployed = nullptr);

    // AI Quota HTTP fetch & JSON parser (/api/status and POST /api/refresh)
    static bool parseQuotaSnapshotJson(const std::string& jsonBody,
                                       AimonQuotaSnapshot& outSnap,
                                       std::string& outError);
    static bool fetchQuotaSnapshot(const std::string& host,
                                   int webPort,
                                   bool triggerRefresh,
                                   AimonQuotaSnapshot& outSnap,
                                   int timeoutMs = 3000);
    static EdaBridgeTelemetrySnapshot mergeTelemetrySnapshots(
        const EdaBridgeTelemetrySnapshot& localSnap,
        const AimonQuotaSnapshot& quotaSnap,
        int64_t minEpoch = 0);
    static std::string formatTrayTooltipSummary(const AimonQuotaSnapshot& snap,
                                                bool eeConn = false,
                                                bool kcConn = false,
                                                bool fcConn = false);
};

} // namespace aimon

#endif // AIMON_EDA_BRIDGE_APP_CONFIG_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
