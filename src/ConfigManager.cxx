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
#include <libconfig.h++>

namespace fs = std::filesystem;

namespace {

bool readSetting(const libconfig::Setting& parent, const char* name, int& val) {
    if (!parent.exists(name)) return false;
    const libconfig::Setting& s = parent[name];
    if (s.getType() != libconfig::Setting::TypeInt) {
        throw libconfig::SettingTypeException(s);
    }
    val = s;
    return true;
}

bool readSetting(const libconfig::Setting& parent, const char* name, bool& val) {
    if (!parent.exists(name)) return false;
    const libconfig::Setting& s = parent[name];
    if (s.getType() != libconfig::Setting::TypeBoolean) {
        throw libconfig::SettingTypeException(s);
    }
    val = s;
    return true;
}

bool readSetting(const libconfig::Setting& parent, const char* name, std::string& val) {
    if (!parent.exists(name)) return false;
    const libconfig::Setting& s = parent[name];
    if (s.getType() != libconfig::Setting::TypeString) {
        throw libconfig::SettingTypeException(s);
    }
    val = (const char*)s;
    return true;
}

bool readSetting(const libconfig::Setting& parent, const char* name, double& val) {
    if (!parent.exists(name)) return false;
    const libconfig::Setting& s = parent[name];
    if (s.getType() == libconfig::Setting::TypeFloat) {
        val = s;
    } else if (s.getType() == libconfig::Setting::TypeInt) {
        val = static_cast<double>(static_cast<int>(s));
    } else {
        throw libconfig::SettingTypeException(s);
    }
    return true;
}

} // anonymous namespace

namespace aimon {

nlohmann::json AimonConfig::toJson() const {
    nlohmann::json j = {
        {"polling", {
            {"base_interval_sec", polling.baseIntervalSec},
            {"idle_interval_sec", polling.idleIntervalSec},
            {"active_interval_sec", polling.activeIntervalSec}
        }},
        {"web", {
            {"host", web.host},
            {"port", web.port},
            {"endpoints_enabled", web.endpointsEnabled}
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
            {"csrf_token", antigravity.csrfToken},
            {"default_model", antigravity.defaultModel}
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
        {"supervisor", {
            {"enabled", supervisor.enabled},
            {"auto_restart", supervisor.autoRestart},
            {"probe_interval_sec", supervisor.probeIntervalSec},
            {"probe_timeout_sec", supervisor.probeTimeoutSec},
            {"probe_timeout_ms", supervisor.probeTimeoutMs},
            {"crash_loop_window_sec", supervisor.crashLoopWindowSec},
            {"crash_loop_max_retries", supervisor.crashLoopMaxRetries},
            {"backoff_initial_sec", supervisor.backoffInitialSec},
            {"backoff_max_sec", supervisor.backoffMaxSec},
            {"max_db_quarantine_versions", supervisor.maxDbQuarantineVersions},
            {"safe_mode_port", supervisor.safeModePort},
            {"prev_binary_path", supervisor.prevBinaryPath}
        }},
        {"gemini", {
            {"enabled", gemini.enabled},
            {"api_key", gemini.apiKey},
            {"model", gemini.model},
            {"max_tokens", gemini.maxTokens},
            {"temperature", gemini.temperature},
            {"prompt_template", gemini.promptTemplate},
            {"incident_log_dir", gemini.incidentLogDir}
        }}
    };

    nlohmann::json svcArray = nlohmann::json::array();
    for (const auto& svc : services) {
        svcArray.push_back({
            {"id", svc.id.empty() ? svc.name : svc.id},
            {"name", svc.name},
            {"host", svc.host},
            {"port", svc.port},
            {"secondary_port", svc.secondaryPort},
            {"enabled", svc.enabled},
            {"probe_interval_ms", svc.probeIntervalMs},
            {"probe_timeout_ms", svc.probeTimeoutMs},
            {"max_restart_retries", svc.maxRestartRetries},
            {"restart_window_sec", svc.restartWindowSec},
            {"start_cmd", svc.startCmd},
            {"stop_cmd", svc.stopCmd},
            {"status_cmd", svc.statusCmd},
            {"pid_file", svc.pidFile},
            {"log_file", svc.logFile}
        });
    }
    j["services"] = svcArray;

    return j;
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
        if (w.contains("endpoints_enabled")) web.endpointsEnabled = w["endpoints_enabled"];
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
        if (a.contains("default_model")) antigravity.defaultModel = a["default_model"];
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

    if (j.contains("supervisor")) {
        const auto& s = j["supervisor"];
        if (s.contains("enabled")) supervisor.enabled = s["enabled"];
        if (s.contains("auto_restart")) supervisor.autoRestart = s["auto_restart"];
        if (s.contains("probe_interval_sec")) supervisor.probeIntervalSec = s["probe_interval_sec"];
        if (s.contains("probe_timeout_sec")) supervisor.probeTimeoutSec = s["probe_timeout_sec"];
        if (s.contains("probe_timeout_ms")) supervisor.probeTimeoutMs = s["probe_timeout_ms"];
        if (s.contains("crash_loop_window_sec")) supervisor.crashLoopWindowSec = s["crash_loop_window_sec"];
        if (s.contains("crash_loop_max_retries")) supervisor.crashLoopMaxRetries = s["crash_loop_max_retries"];
        if (s.contains("backoff_initial_sec")) supervisor.backoffInitialSec = s["backoff_initial_sec"];
        if (s.contains("backoff_max_sec")) supervisor.backoffMaxSec = s["backoff_max_sec"];
        if (s.contains("max_db_quarantine_versions")) supervisor.maxDbQuarantineVersions = s["max_db_quarantine_versions"];
        if (s.contains("safe_mode_port")) supervisor.safeModePort = s["safe_mode_port"];
        if (s.contains("prev_binary_path")) supervisor.prevBinaryPath = s["prev_binary_path"];
    }

    if (j.contains("services") && j["services"].is_array()) {
        services.clear();
        for (const auto& s : j["services"]) {
            SupervisedServiceConfig svc;
            if (s.contains("name")) svc.name = s["name"];
            if (s.contains("id")) svc.id = s["id"];
            else svc.id = svc.name;
            if (s.contains("host")) svc.host = s["host"];
            if (s.contains("port")) svc.port = s["port"];
            if (s.contains("secondary_port")) svc.secondaryPort = s["secondary_port"];
            if (s.contains("enabled")) svc.enabled = s["enabled"];
            if (s.contains("probe_interval_ms")) svc.probeIntervalMs = s["probe_interval_ms"];
            if (s.contains("probe_timeout_ms")) svc.probeTimeoutMs = s["probe_timeout_ms"];
            if (s.contains("max_restart_retries")) svc.maxRestartRetries = s["max_restart_retries"];
            if (s.contains("restart_window_sec")) svc.restartWindowSec = s["restart_window_sec"];
            if (s.contains("start_cmd")) svc.startCmd = s["start_cmd"];
            if (s.contains("stop_cmd")) svc.stopCmd = s["stop_cmd"];
            if (s.contains("status_cmd")) svc.statusCmd = s["status_cmd"];
            if (s.contains("pid_file")) svc.pidFile = s["pid_file"];
            if (s.contains("log_file")) svc.logFile = s["log_file"];
            services.push_back(svc);
        }
    }

    if (j.contains("gemini")) {
        const auto& g = j["gemini"];
        if (g.contains("enabled")) gemini.enabled = g["enabled"];
        if (g.contains("api_key")) gemini.apiKey = g["api_key"];
        if (g.contains("model")) gemini.model = g["model"];
        if (g.contains("max_tokens")) gemini.maxTokens = g["max_tokens"];
        if (g.contains("temperature")) gemini.temperature = g["temperature"];
        if (g.contains("prompt_template")) gemini.promptTemplate = g["prompt_template"];
        if (g.contains("incident_log_dir")) gemini.incidentLogDir = g["incident_log_dir"];
    }
}

ConfigManager::ConfigManager() {
    std::string libCfg = PathUtils::getDefaultLibConfigFilePath();
    std::string jsonCfg = PathUtils::getDefaultConfigFilePath();
    if (fs::exists(libCfg)) {
        _configFilePath = libCfg;
    } else if (fs::exists(jsonCfg)) {
        _configFilePath = jsonCfg;
    } else {
        _configFilePath = libCfg;
    }

    if (_config.history.dbPath.empty()) {
        _config.history.dbPath = PathUtils::getDefaultHistoryDbPath();
    }
    if (_config.cursor.dbPath.empty()) {
        _config.cursor.dbPath = PathUtils::getDefaultCursorDbPath();
    }
}

bool ConfigManager::loadLibConfig(const std::string& customPath) {
    std::string targetPath = customPath.empty() ? PathUtils::getDefaultLibConfigFilePath() : PathUtils::expandHome(customPath);
    _lastErrorMessage.clear();

    if (!fs::exists(targetPath)) {
        _lastErrorMessage = "Configuration file does not exist: " + targetPath;
        return false;
    }

    libconfig::Config cfg;
    try {
        cfg.readFile(targetPath.c_str());
    } catch (const libconfig::FileIOException&) {
        _lastErrorMessage = "I/O error reading configuration file: " + targetPath;
        std::cerr << "[ConfigManager] " << _lastErrorMessage << std::endl;
        return false;
    } catch (const libconfig::ParseException& pex) {
        _lastErrorMessage = "Parse error in " + std::string(pex.getFile() ? pex.getFile() : targetPath)
            + ":" + std::to_string(pex.getLine()) + " - " + pex.getError();
        std::cerr << "[ConfigManager] " << _lastErrorMessage << std::endl;
        return false;
    }

    try {
        const libconfig::Setting& root = cfg.getRoot();

        if (root.exists("supervisor")) {
            const libconfig::Setting& sup = root["supervisor"];
            readSetting(sup, "enabled", _config.supervisor.enabled);
            readSetting(sup, "auto_restart", _config.supervisor.autoRestart);
            readSetting(sup, "probe_interval_sec", _config.supervisor.probeIntervalSec);
            readSetting(sup, "probe_timeout_sec", _config.supervisor.probeTimeoutSec);
            readSetting(sup, "probe_timeout_ms", _config.supervisor.probeTimeoutMs);
            readSetting(sup, "crash_loop_window_sec", _config.supervisor.crashLoopWindowSec);
            readSetting(sup, "crash_loop_max_retries", _config.supervisor.crashLoopMaxRetries);
            readSetting(sup, "backoff_initial_sec", _config.supervisor.backoffInitialSec);
            readSetting(sup, "backoff_max_sec", _config.supervisor.backoffMaxSec);
            readSetting(sup, "max_db_quarantine_versions", _config.supervisor.maxDbQuarantineVersions);
            readSetting(sup, "safe_mode_port", _config.supervisor.safeModePort);
            readSetting(sup, "prev_binary_path", _config.supervisor.prevBinaryPath);
        }

        if (root.exists("services")) {
            const libconfig::Setting& sList = root["services"];
            if (sList.isList() || sList.isArray()) {
                _config.services.clear();
                int count = sList.getLength();
                for (int i = 0; i < count; ++i) {
                    const libconfig::Setting& sElem = sList[i];
                    SupervisedServiceConfig svc;
                    readSetting(sElem, "name", svc.name);
                    readSetting(sElem, "id", svc.id);
                    if (svc.id.empty()) {
                        svc.id = svc.name;
                    }
                    readSetting(sElem, "host", svc.host);
                    readSetting(sElem, "port", svc.port);
                    readSetting(sElem, "secondary_port", svc.secondaryPort);
                    readSetting(sElem, "enabled", svc.enabled);
                    readSetting(sElem, "probe_interval_ms", svc.probeIntervalMs);
                    readSetting(sElem, "probe_timeout_ms", svc.probeTimeoutMs);
                    readSetting(sElem, "max_restart_retries", svc.maxRestartRetries);
                    readSetting(sElem, "restart_window_sec", svc.restartWindowSec);
                    readSetting(sElem, "start_cmd", svc.startCmd);
                    readSetting(sElem, "stop_cmd", svc.stopCmd);
                    readSetting(sElem, "status_cmd", svc.statusCmd);
                    readSetting(sElem, "pid_file", svc.pidFile);
                    readSetting(sElem, "log_file", svc.logFile);
                    _config.services.push_back(svc);
                }
            }
        }

        if (root.exists("gemini")) {
            const libconfig::Setting& gem = root["gemini"];
            readSetting(gem, "enabled", _config.gemini.enabled);
            readSetting(gem, "api_key", _config.gemini.apiKey);
            readSetting(gem, "model", _config.gemini.model);
            readSetting(gem, "max_tokens", _config.gemini.maxTokens);
            readSetting(gem, "temperature", _config.gemini.temperature);
            readSetting(gem, "prompt_template", _config.gemini.promptTemplate);
            readSetting(gem, "incident_log_dir", _config.gemini.incidentLogDir);
        }

        if (root.exists("polling")) {
            const libconfig::Setting& pol = root["polling"];
            readSetting(pol, "base_interval_sec", _config.polling.baseIntervalSec);
            readSetting(pol, "idle_interval_sec", _config.polling.idleIntervalSec);
            readSetting(pol, "active_interval_sec", _config.polling.activeIntervalSec);
        }

        if (root.exists("web")) {
            const libconfig::Setting& wb = root["web"];
            readSetting(wb, "host", _config.web.host);
            readSetting(wb, "port", _config.web.port);
            readSetting(wb, "endpoints_enabled", _config.web.endpointsEnabled);
        }

        if (root.exists("mqtt")) {
            const libconfig::Setting& mq = root["mqtt"];
            readSetting(mq, "enabled", _config.mqtt.enabled);
            readSetting(mq, "broker", _config.mqtt.broker);
            readSetting(mq, "port", _config.mqtt.port);
            readSetting(mq, "username", _config.mqtt.username);
            readSetting(mq, "password", _config.mqtt.password);
            readSetting(mq, "topic_prefix", _config.mqtt.topicPrefix);
            readSetting(mq, "discovery_prefix", _config.mqtt.discoveryPrefix);
            readSetting(mq, "retain", _config.mqtt.retain);
        }

        if (root.exists("history")) {
            const libconfig::Setting& hist = root["history"];
            readSetting(hist, "enabled", _config.history.enabled);
            readSetting(hist, "db_path", _config.history.dbPath);
        }

        if (root.exists("antigravity")) {
            const libconfig::Setting& ag = root["antigravity"];
            readSetting(ag, "auto_discover", _config.antigravity.autoDiscover);
            readSetting(ag, "port", _config.antigravity.port);
            readSetting(ag, "csrf_token", _config.antigravity.csrfToken);
            readSetting(ag, "default_model", _config.antigravity.defaultModel);
        }

        if (root.exists("cursor")) {
            const libconfig::Setting& cr = root["cursor"];
            readSetting(cr, "auto_discover", _config.cursor.autoDiscover);
            readSetting(cr, "db_path", _config.cursor.dbPath);
            readSetting(cr, "access_token", _config.cursor.accessToken);
        }

        if (root.exists("gateway")) {
            const libconfig::Setting& gw = root["gateway"];
            readSetting(gw, "enabled", _config.gateway.enabled);
            readSetting(gw, "host", _config.gateway.host);
            readSetting(gw, "port", _config.gateway.port);
        }

    } catch (const libconfig::SettingTypeException& stex) {
        _lastErrorMessage = "Setting type mismatch at " + std::string(stex.getPath());
        std::cerr << "[ConfigManager] " << _lastErrorMessage << std::endl;
        return false;
    } catch (const libconfig::SettingException& sex) {
        _lastErrorMessage = "Setting error at " + std::string(sex.getPath());
        std::cerr << "[ConfigManager] " << _lastErrorMessage << std::endl;
        return false;
    }

    _configFilePath = targetPath;
    applyEnvironmentOverrides();
    return true;
}

bool ConfigManager::saveLibConfig(const std::string& customPath) const {
    std::string targetPath = customPath.empty() ? PathUtils::getDefaultLibConfigFilePath() : PathUtils::expandHome(customPath);
    std::string dir = fs::path(targetPath).parent_path().string();
    if (!dir.empty()) {
        PathUtils::ensureDirectoryExists(dir);
    }

    try {
        libconfig::Config cfg;
        libconfig::Setting& root = cfg.getRoot();

        // supervisor
        libconfig::Setting& sup = root.add("supervisor", libconfig::Setting::TypeGroup);
        sup.add("enabled", libconfig::Setting::TypeBoolean) = _config.supervisor.enabled;
        sup.add("auto_restart", libconfig::Setting::TypeBoolean) = _config.supervisor.autoRestart;
        sup.add("probe_interval_sec", libconfig::Setting::TypeInt) = _config.supervisor.probeIntervalSec;
        sup.add("probe_timeout_sec", libconfig::Setting::TypeInt) = _config.supervisor.probeTimeoutSec;
        sup.add("probe_timeout_ms", libconfig::Setting::TypeInt) = _config.supervisor.probeTimeoutMs;
        sup.add("crash_loop_window_sec", libconfig::Setting::TypeInt) = _config.supervisor.crashLoopWindowSec;
        sup.add("crash_loop_max_retries", libconfig::Setting::TypeInt) = _config.supervisor.crashLoopMaxRetries;
        sup.add("backoff_initial_sec", libconfig::Setting::TypeInt) = _config.supervisor.backoffInitialSec;
        sup.add("backoff_max_sec", libconfig::Setting::TypeInt) = _config.supervisor.backoffMaxSec;
        sup.add("max_db_quarantine_versions", libconfig::Setting::TypeInt) = _config.supervisor.maxDbQuarantineVersions;
        sup.add("safe_mode_port", libconfig::Setting::TypeInt) = _config.supervisor.safeModePort;
        sup.add("prev_binary_path", libconfig::Setting::TypeString) = _config.supervisor.prevBinaryPath;

        // services
        libconfig::Setting& sList = root.add("services", libconfig::Setting::TypeList);
        for (const auto& svc : _config.services) {
            libconfig::Setting& sElem = sList.add(libconfig::Setting::TypeGroup);
            if (!svc.id.empty()) {
                sElem.add("id", libconfig::Setting::TypeString) = svc.id;
            }
            sElem.add("name", libconfig::Setting::TypeString) = svc.name;
            sElem.add("host", libconfig::Setting::TypeString) = svc.host;
            sElem.add("port", libconfig::Setting::TypeInt) = svc.port;
            sElem.add("secondary_port", libconfig::Setting::TypeInt) = svc.secondaryPort;
            sElem.add("enabled", libconfig::Setting::TypeBoolean) = svc.enabled;
            sElem.add("probe_interval_ms", libconfig::Setting::TypeInt) = svc.probeIntervalMs;
            sElem.add("probe_timeout_ms", libconfig::Setting::TypeInt) = svc.probeTimeoutMs;
            sElem.add("max_restart_retries", libconfig::Setting::TypeInt) = svc.maxRestartRetries;
            sElem.add("restart_window_sec", libconfig::Setting::TypeInt) = svc.restartWindowSec;
            sElem.add("start_cmd", libconfig::Setting::TypeString) = svc.startCmd;
            sElem.add("stop_cmd", libconfig::Setting::TypeString) = svc.stopCmd;
            if (!svc.statusCmd.empty()) {
                sElem.add("status_cmd", libconfig::Setting::TypeString) = svc.statusCmd;
            }
            if (!svc.pidFile.empty()) {
                sElem.add("pid_file", libconfig::Setting::TypeString) = svc.pidFile;
            }
            sElem.add("log_file", libconfig::Setting::TypeString) = svc.logFile;
        }

        // gemini
        libconfig::Setting& gem = root.add("gemini", libconfig::Setting::TypeGroup);
        gem.add("enabled", libconfig::Setting::TypeBoolean) = _config.gemini.enabled;
        gem.add("api_key", libconfig::Setting::TypeString) = _config.gemini.apiKey;
        gem.add("model", libconfig::Setting::TypeString) = _config.gemini.model;
        gem.add("max_tokens", libconfig::Setting::TypeInt) = _config.gemini.maxTokens;
        gem.add("temperature", libconfig::Setting::TypeFloat) = _config.gemini.temperature;
        gem.add("prompt_template", libconfig::Setting::TypeString) = _config.gemini.promptTemplate;
        gem.add("incident_log_dir", libconfig::Setting::TypeString) = _config.gemini.incidentLogDir;

        // polling
        libconfig::Setting& pol = root.add("polling", libconfig::Setting::TypeGroup);
        pol.add("base_interval_sec", libconfig::Setting::TypeInt) = _config.polling.baseIntervalSec;
        pol.add("idle_interval_sec", libconfig::Setting::TypeInt) = _config.polling.idleIntervalSec;
        pol.add("active_interval_sec", libconfig::Setting::TypeInt) = _config.polling.activeIntervalSec;

        // web
        libconfig::Setting& wb = root.add("web", libconfig::Setting::TypeGroup);
        wb.add("host", libconfig::Setting::TypeString) = _config.web.host;
        wb.add("port", libconfig::Setting::TypeInt) = _config.web.port;
        wb.add("endpoints_enabled", libconfig::Setting::TypeBoolean) = _config.web.endpointsEnabled;

        // mqtt
        libconfig::Setting& mq = root.add("mqtt", libconfig::Setting::TypeGroup);
        mq.add("enabled", libconfig::Setting::TypeBoolean) = _config.mqtt.enabled;
        mq.add("broker", libconfig::Setting::TypeString) = _config.mqtt.broker;
        mq.add("port", libconfig::Setting::TypeInt) = _config.mqtt.port;
        mq.add("username", libconfig::Setting::TypeString) = _config.mqtt.username;
        mq.add("password", libconfig::Setting::TypeString) = _config.mqtt.password;
        mq.add("topic_prefix", libconfig::Setting::TypeString) = _config.mqtt.topicPrefix;
        mq.add("discovery_prefix", libconfig::Setting::TypeString) = _config.mqtt.discoveryPrefix;
        mq.add("retain", libconfig::Setting::TypeBoolean) = _config.mqtt.retain;

        // history
        libconfig::Setting& hist = root.add("history", libconfig::Setting::TypeGroup);
        hist.add("enabled", libconfig::Setting::TypeBoolean) = _config.history.enabled;
        hist.add("db_path", libconfig::Setting::TypeString) = _config.history.dbPath;

        // antigravity
        libconfig::Setting& ag = root.add("antigravity", libconfig::Setting::TypeGroup);
        ag.add("auto_discover", libconfig::Setting::TypeBoolean) = _config.antigravity.autoDiscover;
        ag.add("port", libconfig::Setting::TypeInt) = _config.antigravity.port;
        ag.add("csrf_token", libconfig::Setting::TypeString) = _config.antigravity.csrfToken;
        ag.add("default_model", libconfig::Setting::TypeString) = _config.antigravity.defaultModel;

        // cursor
        libconfig::Setting& cr = root.add("cursor", libconfig::Setting::TypeGroup);
        cr.add("auto_discover", libconfig::Setting::TypeBoolean) = _config.cursor.autoDiscover;
        cr.add("db_path", libconfig::Setting::TypeString) = _config.cursor.dbPath;
        cr.add("access_token", libconfig::Setting::TypeString) = _config.cursor.accessToken;

        // gateway
        libconfig::Setting& gw = root.add("gateway", libconfig::Setting::TypeGroup);
        gw.add("enabled", libconfig::Setting::TypeBoolean) = _config.gateway.enabled;
        gw.add("host", libconfig::Setting::TypeString) = _config.gateway.host;
        gw.add("port", libconfig::Setting::TypeInt) = _config.gateway.port;

        cfg.writeFile(targetPath.c_str());
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[ConfigManager] Error: failed to write libconfig to "
                  << targetPath << ": " << e.what() << std::endl;
        return false;
    }
}

bool ConfigManager::load(const std::string& customPath) {
    if (!customPath.empty()) {
        std::string expanded = PathUtils::expandHome(customPath);
        if (expanded.length() >= 4 && expanded.substr(expanded.length() - 4) == ".cfg") {
            return loadLibConfig(expanded);
        } else if (expanded.length() >= 5 && expanded.substr(expanded.length() - 5) == ".json") {
            _configFilePath = expanded;
        } else {
            if (fs::exists(expanded + ".cfg")) {
                return loadLibConfig(expanded + ".cfg");
            }
            _configFilePath = expanded;
        }
    } else {
        std::string libCfgPath = PathUtils::getDefaultLibConfigFilePath();
        if (fs::exists(libCfgPath)) {
            return loadLibConfig(libCfgPath);
        }
        std::string jsonCfgPath = PathUtils::getDefaultConfigFilePath();
        if (fs::exists(jsonCfgPath)) {
            _configFilePath = jsonCfgPath;
        } else {
            _configFilePath = libCfgPath;
            saveLibConfig(_configFilePath);
            applyEnvironmentOverrides();
            return true;
        }
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
        saveLibConfig(PathUtils::getDefaultLibConfigFilePath());
    }

    applyEnvironmentOverrides();
    return true;
}

bool ConfigManager::save(const std::string& customPath) const {
    std::string targetPath = customPath.empty() ? _configFilePath : PathUtils::expandHome(customPath);
    if (targetPath.length() >= 4 && targetPath.substr(targetPath.length() - 4) == ".cfg") {
        return saveLibConfig(targetPath);
    }

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

    const char* geminiKey = std::getenv("GEMINI_API_KEY");
    if (geminiKey && *geminiKey) {
        _config.gemini.apiKey = geminiKey;
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
