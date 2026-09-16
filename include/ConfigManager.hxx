/*
 * ConfigManager.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_CONFIG_MANAGER_HXX
#define AIMON_CONFIG_MANAGER_HXX

#include <string>
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

struct AgentRunnerConfig {
    std::string readCommand;
    std::string writeCommand;
    std::string scriptBridge;
    int timeoutSeconds = 300;
};

struct CollaborationConfig {
    bool autoDrive = true;
    // Only applies while a desktop agent is genuinely blocked in
    // agent_wait_turn. With no live waiter the proxy starts immediately.
    int liveWaiterGraceSeconds = 120;
    // Pause before re-spawning a proxy that could not run at all, so an
    // unusable CLI does not respawn every poll cycle.
    int runnerBackoffSeconds = 60;
    int writeTimeoutSeconds = 600;
    int readTimeoutSeconds = 300;
    int maxTurns = 8;
    int maxRetriesPerTurn = 1;
    int minCursorQuota = 10;
    std::map<std::string, AgentRunnerConfig> runners;
};

struct AimonConfig {
    PollingConfig polling;
    WebConfig web;
    MqttConfig mqtt;
    HistoryConfig history;
    AntigravityConfig antigravity;
    CursorConfig cursor;
    GatewayConfig gateway;
    CollaborationConfig collaboration;

    nlohmann::json toJson() const;
    void fromJson(const nlohmann::json& j);
};

class ConfigManager {
public:
    ConfigManager();

    bool load(const std::string& customPath = "");
    bool save(const std::string& customPath = "") const;

    const AimonConfig& getConfig() const {
        return _config;
    }

    AimonConfig& getConfig() {
        return _config;
    }

    void applyEnvironmentOverrides();

private:
    AimonConfig _config;
    std::string _configFilePath;
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
