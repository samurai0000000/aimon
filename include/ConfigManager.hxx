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

struct GatewayConfig {
    bool enabled = true;
    std::string host = "0.0.0.0";
    int port = 3885;
};

struct SupervisedServiceConfig {
    std::string id;
    std::string name;
    std::string host = "127.0.0.1";
    int port = 0;
    int secondaryPort = 0;
    bool enabled = true;
    int probeIntervalMs = 5000;
    int probeTimeoutMs = 2000;
    int maxRestartRetries = 5;
    int restartWindowSec = 60;
    std::string startCmd;
    std::string stopCmd;
    std::string statusCmd;
    std::string pidFile;
    std::string logFile;
};

struct SupervisorConfig {
    bool enabled = true;
    bool autoRestart = true;
    int probeIntervalSec = 5;
    int probeTimeoutSec = 2;
    int probeTimeoutMs = 1000;
    int crashLoopWindowSec = 60;
    int crashLoopMaxRetries = 5;
    int backoffInitialSec = 1;
    int backoffMaxSec = 30;
    int maxDbQuarantineVersions = 5;
    int safeModePort = 3889;
    std::string prevBinaryPath = "/usr/local/bin/aimon.prev";
};

struct GeminiConfig {
    bool enabled = false;
    std::string apiKey;
    std::string model = "gemini-2.5-flash";
    int maxTokens = 2048;
    double temperature = 0.2;
    std::string promptTemplate = "Analyze the following crash telemetry and provide root-cause diagnostics:\n\n${INCIDENT_REPORT}";
    std::string incidentLogDir = "~/.local/state/aimon/incidents";
};

struct AimonConfig {
    PollingConfig polling;
    WebConfig web;
    MqttConfig mqtt;
    HistoryConfig history;
    AntigravityConfig antigravity;
    CursorConfig cursor;
    GatewayConfig gateway;
    SupervisorConfig supervisor;
    std::vector<SupervisedServiceConfig> services;
    GeminiConfig gemini;

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
