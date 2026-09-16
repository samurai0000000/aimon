/*
 * ConfigManager.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "ConfigManager.hxx"
#include "PathUtils.hxx"
#include <fstream>
#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

namespace aimon {

nlohmann::json AimonConfig::toJson() const {
    return {
        {"polling", {
            {"base_interval_sec", polling.baseIntervalSec},
            {"idle_interval_sec", polling.idleIntervalSec},
            {"active_interval_sec", polling.activeIntervalSec}
        }},
        {"web", {
            {"host", web.host},
            {"port", web.port}
        }},
        {"mqtt", {
            {"enabled", mqtt.enabled},
            {"broker", mqtt.broker},
            {"port", mqtt.port},
            {"username", mqtt.username},
            {"password", mqtt.password},
            {"topic_prefix", mqtt.topicPrefix},
            {"discovery_prefix", mqtt.discoveryPrefix},
            {"retain", mqtt.retain}
        }},
        {"history", {
            {"enabled", history.enabled},
            {"db_path", history.dbPath}
        }},
        {"antigravity", {
            {"auto_discover", antigravity.autoDiscover},
            {"port", antigravity.port},
            {"csrf_token", antigravity.csrfToken}
        }},
        {"cursor", {
            {"auto_discover", cursor.autoDiscover},
            {"db_path", cursor.dbPath},
            {"access_token", cursor.accessToken}
        }},
        {"gateway", {
            {"enabled", gateway.enabled},
            {"host", gateway.host},
            {"port", gateway.port}
        }},
        {"collaboration", [this]() {
            nlohmann::json rObj = nlohmann::json::object();
            for (const auto& kv : collaboration.runners) {
                rObj[kv.first] = {
                    {"read_command", kv.second.readCommand},
                    {"write_command", kv.second.writeCommand},
                    {"script_bridge", kv.second.scriptBridge},
                    {"timeout_seconds", kv.second.timeoutSeconds}
                };
            }
            return nlohmann::json{
                {"auto_drive", collaboration.autoDrive},
                {"live_waiter_grace_seconds", collaboration.liveWaiterGraceSeconds},
                {"runner_backoff_seconds", collaboration.runnerBackoffSeconds},
                {"write_timeout_seconds", collaboration.writeTimeoutSeconds},
                {"read_timeout_seconds", collaboration.readTimeoutSeconds},
                {"max_turns", collaboration.maxTurns},
                {"max_retries_per_turn", collaboration.maxRetriesPerTurn},
                {"min_cursor_quota", collaboration.minCursorQuota},
                {"runners", rObj}
            };
        }()}
    };
}

void AimonConfig::fromJson(const nlohmann::json& j) {
    if (j.contains("polling")) {
        const auto& p = j["polling"];
        if (p.contains("base_interval_sec")) polling.baseIntervalSec = p["base_interval_sec"];
        if (p.contains("idle_interval_sec")) polling.idleIntervalSec = p["idle_interval_sec"];
        if (p.contains("active_interval_sec")) polling.activeIntervalSec = p["active_interval_sec"];
    }

    if (j.contains("web")) {
        const auto& w = j["web"];
        if (w.contains("host")) web.host = w["host"];
        if (w.contains("port")) web.port = w["port"];
    }

    if (j.contains("mqtt")) {
        const auto& m = j["mqtt"];
        if (m.contains("enabled")) mqtt.enabled = m["enabled"];
        if (m.contains("broker")) mqtt.broker = m["broker"];
        if (m.contains("port")) mqtt.port = m["port"];
        if (m.contains("username")) mqtt.username = m["username"];
        if (m.contains("password")) mqtt.password = m["password"];
        if (m.contains("topic_prefix")) mqtt.topicPrefix = m["topic_prefix"];
        if (m.contains("discovery_prefix")) mqtt.discoveryPrefix = m["discovery_prefix"];
        if (m.contains("retain")) mqtt.retain = m["retain"];
    }

    if (j.contains("history")) {
        const auto& h = j["history"];
        if (h.contains("enabled")) history.enabled = h["enabled"];
        if (h.contains("db_path")) history.dbPath = h["db_path"];
    }

    if (j.contains("antigravity")) {
        const auto& a = j["antigravity"];
        if (a.contains("auto_discover")) antigravity.autoDiscover = a["auto_discover"];
        if (a.contains("port")) antigravity.port = a["port"];
        if (a.contains("csrf_token")) antigravity.csrfToken = a["csrf_token"];
    }

    if (j.contains("cursor")) {
        const auto& c = j["cursor"];
        if (c.contains("auto_discover")) cursor.autoDiscover = c["auto_discover"];
        if (c.contains("db_path")) cursor.dbPath = c["db_path"];
        if (c.contains("access_token")) cursor.accessToken = c["access_token"];
    }

    if (j.contains("gateway")) {
        const auto& g = j["gateway"];
        if (g.contains("enabled")) gateway.enabled = g["enabled"];
        if (g.contains("host")) gateway.host = g["host"];
        if (g.contains("port")) gateway.port = g["port"];
    }

    if (j.contains("collaboration")) {
        const auto& col = j["collaboration"];
        if (col.contains("auto_drive")) collaboration.autoDrive = col["auto_drive"];
        if (col.contains("claim_window_seconds")) collaboration.liveWaiterGraceSeconds = col["claim_window_seconds"];
        if (col.contains("live_waiter_grace_seconds")) collaboration.liveWaiterGraceSeconds = col["live_waiter_grace_seconds"];
        if (col.contains("runner_backoff_seconds")) collaboration.runnerBackoffSeconds = col["runner_backoff_seconds"];
        if (col.contains("write_timeout_seconds")) collaboration.writeTimeoutSeconds = col["write_timeout_seconds"];
        if (col.contains("read_timeout_seconds")) collaboration.readTimeoutSeconds = col["read_timeout_seconds"];
        if (col.contains("max_turns")) collaboration.maxTurns = col["max_turns"];
        if (col.contains("max_retries_per_turn")) collaboration.maxRetriesPerTurn = col["max_retries_per_turn"];
        if (col.contains("min_cursor_quota")) collaboration.minCursorQuota = col["min_cursor_quota"];
        if (col.contains("runners") && col["runners"].is_object()) {
            for (auto it = col["runners"].begin(); it != col["runners"].end(); ++it) {
                AgentRunnerConfig r;
                if (it.value().contains("read_command")) r.readCommand = it.value()["read_command"];
                if (it.value().contains("write_command")) r.writeCommand = it.value()["write_command"];
                if (it.value().contains("script_bridge")) r.scriptBridge = it.value()["script_bridge"];
                if (it.value().contains("timeout_seconds")) r.timeoutSeconds = it.value()["timeout_seconds"];
                collaboration.runners[it.key()] = r;
            }
        }
    }
}

ConfigManager::ConfigManager() {
    _configFilePath = PathUtils::getDefaultConfigFilePath();
    if (_config.history.dbPath.empty()) {
        _config.history.dbPath = PathUtils::getDefaultHistoryDbPath();
    }
    if (_config.cursor.dbPath.empty()) {
        _config.cursor.dbPath = PathUtils::getDefaultCursorDbPath();
    }
}

bool ConfigManager::load(const std::string& customPath) {
    if (!customPath.empty()) {
        _configFilePath = PathUtils::expandHome(customPath);
    }

    if (fs::exists(_configFilePath)) {
        try {
            std::ifstream file(_configFilePath);
            if (file.is_open()) {
                nlohmann::json j;
                file >> j;
                _config.fromJson(j);
                applyEnvironmentOverrides();
                return true;
            }
        } catch (const std::exception& e) {
            std::cerr << "[ConfigManager] Warning: failed to parse "
                      << _configFilePath << ": " << e.what() << std::endl;
        }
    } else {
        // Auto-generate default configuration file if missing
        save(_configFilePath);
    }

    applyEnvironmentOverrides();
    return true;
}

bool ConfigManager::save(const std::string& customPath) const {
    std::string targetPath = customPath.empty() ? _configFilePath : PathUtils::expandHome(customPath);
    std::string dir = fs::path(targetPath).parent_path().string();
    if (!dir.empty()) {
        PathUtils::ensureDirectoryExists(dir);
    }

    try {
        std::ofstream file(targetPath);
        if (file.is_open()) {
            file << _config.toJson().dump(2) << std::endl;
            return true;
        }
    } catch (const std::exception& e) {
        std::cerr << "[ConfigManager] Error: failed to write config to "
                  << targetPath << ": " << e.what() << std::endl;
    }

    return false;
}

void ConfigManager::applyEnvironmentOverrides() {
    const char* cursorToken = std::getenv("CURSOR_ACCESS_TOKEN");
    if (!cursorToken) {
        cursorToken = std::getenv("CURSOR_API_KEY");
    }
    if (cursorToken && *cursorToken) {
        _config.cursor.accessToken = cursorToken;
    }

    const char* agToken = std::getenv("ANTIGRAVITY_CSRF_TOKEN");
    if (agToken && *agToken) {
        _config.antigravity.csrfToken = agToken;
    }

    const char* mqttBroker = std::getenv("MQTT_BROKER");
    if (mqttBroker && *mqttBroker) {
        _config.mqtt.broker = mqttBroker;
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
