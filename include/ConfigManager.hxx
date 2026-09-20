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

struct AimonConfig {
    PollingConfig polling;
    WebConfig web;
    MqttConfig mqtt;
    HistoryConfig history;
    AntigravityConfig antigravity;
    CursorConfig cursor;
    GatewayConfig gateway;

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
