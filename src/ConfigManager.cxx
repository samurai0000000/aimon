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
        {"gateway", gateway.toJson()}
    };

    nlohmann::json claudeAccounts = nlohmann::json::array();
    for (const auto& a : claude.accounts) {
        claudeAccounts.push_back({
            {"name", a.name},
            {"config_dir", a.configDir},
            {"spend_limit_usd", a.spendLimitUsd},
            {"cycle_reset_day", a.cycleResetDay}
        });
    }
    j["claude"] = {
        {"enabled", claude.enabled},
        {"window_days", claude.windowDays},
        {"retention_days", claude.retentionDays},
        {"pricing_url", claude.pricingUrl},
        {"pricing_refresh_hours", claude.pricingRefreshHours},
        {"accounts", claudeAccounts}
    };

    j["insights"] = {
        {"enabled", insights.enabled},
        {"db_path", insights.dbPath},
        {"retention_days", insights.retentionDays},
        {"refresh_interval_sec", insights.refreshIntervalSec},
        {"long_context_threshold_tokens", insights.longContextThresholdTokens},
        {"compact_context_threshold_tokens", insights.compactContextThresholdTokens},
        {"compact_continue_requests", insights.compactContinueRequests},
        {"subagent_share_threshold", insights.subagentShareThreshold},
        {"long_session_hours", insights.longSessionHours},
        {"cache_efficiency_threshold", insights.cacheEfficiencyThreshold},
        {"cache_efficiency_min_requests", insights.cacheEfficiencyMinRequests},
        {"error_retry_streak_threshold", insights.errorRetryStreakThreshold},
        {"idle_gap_seconds", insights.idleGapSeconds},
        {"burn_rate_alert_fraction", insights.burnRateAlertFraction},
        {"short_prompt_max_prose_chars", insights.shortPromptMaxProseChars},
        {"drip_prompt_max_prose_chars", insights.dripPromptMaxProseChars},
        {"drip_prompts_per_hour", insights.dripPromptsPerHour},
        {"quick_reprompt_seconds", insights.quickRepromptSeconds},
        {"coaching_min_sample_size", insights.coachingMinSampleSize}
    };

    j["eda"] = eda.toJson(false);

    return j;
}

nlohmann::json EdaBridgeConfig::toJson(bool redactSecret) const {
    return {
        {"enabled", enabled},
        {"mode", mode},
        {"host", host},
        {"port", port},
        {"auth_secret", redactSecret ? (authSecret.empty() ? "" : "***") : authSecret},
        {"health_poll_interval_sec", healthPollIntervalSec},
        {"timeout_ms", timeoutMs},
        {"max_output_bytes", maxOutputBytes},
        {"accelerators", {
            {"enabled", accelerators.enabled},
            {"sa_placement_iterations", accelerators.saPlacementIterations},
            {"autoroute_max_ripup_passes", accelerators.autorouteMaxRipupPasses},
            {"autoroute_grid_mm", accelerators.autorouteGridMm},
            {"prefer_jlcpcb_basic_e_series", accelerators.preferJlcpcbBasicESeries},
            {"default_copper_weight_oz", accelerators.defaultCopperWeightOz},
            {"default_filament_material", accelerators.defaultFilamentMaterial},
            {"default_gear_pressure_angle_deg", accelerators.defaultGearPressureAngleDeg}
        }},
        {"easyeda", {
            {"enabled", easyeda.enabled}
        }},
        {"kicad", {
            {"enabled", kicad.enabled}
        }},
        {"freecad", {
            {"enabled", freecad.enabled}
        }}
    };
}

void EdaBridgeConfig::fromJson(const nlohmann::json& j) {
    if (!j.is_object()) return;
    if (j.contains("enabled") && j["enabled"].is_boolean()) enabled = j["enabled"];
    if (j.contains("mode") && j["mode"].is_string()) mode = j["mode"];
    if (j.contains("host") && j["host"].is_string()) host = j["host"];
    if (j.contains("port") && j["port"].is_number_integer()) port = j["port"];
    if (j.contains("auth_secret") && j["auth_secret"].is_string()) {
        std::string sec = j["auth_secret"];
        if (sec != "***") {
            authSecret = sec;
        }
    }
    if (j.contains("health_poll_interval_sec") && j["health_poll_interval_sec"].is_number_integer()) {
        healthPollIntervalSec = j["health_poll_interval_sec"];
    }
    if (j.contains("timeout_ms") && j["timeout_ms"].is_number_integer()) timeoutMs = j["timeout_ms"];
    if (j.contains("max_output_bytes") && j["max_output_bytes"].is_number_integer()) maxOutputBytes = j["max_output_bytes"];

    if (j.contains("accelerators") && j["accelerators"].is_object()) {
        const auto& a = j["accelerators"];
        if (a.contains("enabled") && a["enabled"].is_boolean()) accelerators.enabled = a["enabled"];
        if (a.contains("sa_placement_iterations") && a["sa_placement_iterations"].is_number_integer()) {
            accelerators.saPlacementIterations = a["sa_placement_iterations"];
        }
        if (a.contains("autoroute_max_ripup_passes") && a["autoroute_max_ripup_passes"].is_number_integer()) {
            accelerators.autorouteMaxRipupPasses = a["autoroute_max_ripup_passes"];
        }
        if (a.contains("autoroute_grid_mm") && a["autoroute_grid_mm"].is_number()) {
            accelerators.autorouteGridMm = a["autoroute_grid_mm"];
        }
        if (a.contains("prefer_jlcpcb_basic_e_series") && a["prefer_jlcpcb_basic_e_series"].is_boolean()) {
            accelerators.preferJlcpcbBasicESeries = a["prefer_jlcpcb_basic_e_series"];
        }
        if (a.contains("default_copper_weight_oz") && a["default_copper_weight_oz"].is_number()) {
            accelerators.defaultCopperWeightOz = a["default_copper_weight_oz"];
        }
        if (a.contains("default_filament_material") && a["default_filament_material"].is_string()) {
            accelerators.defaultFilamentMaterial = a["default_filament_material"];
        }
        if (a.contains("default_gear_pressure_angle_deg") && a["default_gear_pressure_angle_deg"].is_number()) {
            accelerators.defaultGearPressureAngleDeg = a["default_gear_pressure_angle_deg"];
        }
    }

    if (j.contains("easyeda") && j["easyeda"].is_object()) {
        const auto& e = j["easyeda"];
        if (e.contains("enabled") && e["enabled"].is_boolean()) easyeda.enabled = e["enabled"];
    }

    if (j.contains("kicad") && j["kicad"].is_object()) {
        const auto& k = j["kicad"];
        if (k.contains("enabled") && k["enabled"].is_boolean()) kicad.enabled = k["enabled"];
    }

    if (j.contains("freecad") && j["freecad"].is_object()) {
        const auto& f = j["freecad"];
        if (f.contains("enabled") && f["enabled"].is_boolean()) freecad.enabled = f["enabled"];
    }
}

bool EdaBridgeConfig::validate(std::string& error) const {
    if (mode != "client" && mode != "gateway" && mode != "bidirectional") {
        error = "eda.mode must be 'client', 'gateway', or 'bidirectional'";
        return false;
    }
    if (port < 1 || port > 65535) {
        error = "eda.port must be between 1 and 65535";
        return false;
    }
    if (healthPollIntervalSec < 5 || healthPollIntervalSec > 3600) {
        error = "eda.health_poll_interval_sec must be between 5 and 3600";
        return false;
    }
    if (timeoutMs < 1000 || timeoutMs > 600000) {
        error = "eda.timeout_ms must be between 1000 and 600000";
        return false;
    }
    if (maxOutputBytes < 65536 || maxOutputBytes > 67108864) {
        error = "eda.max_output_bytes must be between 65536 and 67108864";
        return false;
    }

    if (accelerators.saPlacementIterations < 100 || accelerators.saPlacementIterations > 500000) {
        error = "eda.accelerators.sa_placement_iterations must be between 100 and 500000";
        return false;
    }
    if (accelerators.autorouteMaxRipupPasses < 1 || accelerators.autorouteMaxRipupPasses > 200) {
        error = "eda.accelerators.autoroute_max_ripup_passes must be between 1 and 200";
        return false;
    }
    if (accelerators.autorouteGridMm < 0.05 || accelerators.autorouteGridMm > 2.54) {
        error = "eda.accelerators.autoroute_grid_mm must be between 0.05 and 2.54";
        return false;
    }
    if (accelerators.defaultCopperWeightOz <= 0.0 || accelerators.defaultCopperWeightOz > 10.0) {
        error = "eda.accelerators.default_copper_weight_oz must be between 0.1 and 10.0";
        return false;
    }
    const std::string& mat = accelerators.defaultFilamentMaterial;
    if (mat != "PLA" && mat != "PETG" && mat != "ABS" && mat != "ASA" && mat != "PA-CF") {
        error = "eda.accelerators.default_filament_material must be PLA, PETG, ABS, ASA, or PA-CF";
        return false;
    }
    if (accelerators.defaultGearPressureAngleDeg < 10.0 || accelerators.defaultGearPressureAngleDeg > 35.0) {
        error = "eda.accelerators.default_gear_pressure_angle_deg must be between 10.0 and 35.0";
        return false;
    }

    error.clear();
    return true;
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

    if (j.contains("claude") && j["claude"].is_object()) {
        const auto& c = j["claude"];
        if (c.contains("enabled") && c["enabled"].is_boolean()) claude.enabled = c["enabled"];
        if (c.contains("window_days") && c["window_days"].is_number_integer()) claude.windowDays = c["window_days"];
        if (c.contains("retention_days") && c["retention_days"].is_number_integer()) claude.retentionDays = c["retention_days"];
        if (c.contains("pricing_url") && c["pricing_url"].is_string()) claude.pricingUrl = c["pricing_url"];
        if (c.contains("pricing_refresh_hours") && c["pricing_refresh_hours"].is_number_integer()) {
            claude.pricingRefreshHours = c["pricing_refresh_hours"];
        }
        if (c.contains("accounts") && c["accounts"].is_array()) {
            claude.accounts.clear();
            for (const auto& a : c["accounts"]) {
                if (!a.is_object()) continue;
                ClaudeAccountConfig acct;
                if (a.contains("name") && a["name"].is_string()) acct.name = a["name"];
                if (a.contains("config_dir") && a["config_dir"].is_string()) acct.configDir = a["config_dir"];
                if (a.contains("spend_limit_usd") && a["spend_limit_usd"].is_number()) acct.spendLimitUsd = a["spend_limit_usd"];
                if (a.contains("cycle_reset_day") && a["cycle_reset_day"].is_number_integer()) acct.cycleResetDay = a["cycle_reset_day"];
                claude.accounts.push_back(acct);
            }
        }
    }

    if (j.contains("gateway") && j["gateway"].is_object()) {
        gateway.fromJson(j["gateway"]);
    }

    if (j.contains("diagnostics") && j["diagnostics"].is_object()) {
        const auto& dg = j["diagnostics"];
        bool hasGwRetention = j.contains("gateway") && j["gateway"].is_object() &&
                              j["gateway"].contains("telemetry_retention_days");
        if (!hasGwRetention && dg.contains("telemetry_retention_days") &&
            dg["telemetry_retention_days"].is_number_integer()) {
            gateway.telemetryRetentionDays = dg["telemetry_retention_days"];
        }
    }

    if (j.contains("insights") && j["insights"].is_object()) {
        const auto& ins = j["insights"];
        if (ins.contains("enabled") && ins["enabled"].is_boolean()) insights.enabled = ins["enabled"];
        if (ins.contains("db_path") && ins["db_path"].is_string()) insights.dbPath = ins["db_path"];
        if (ins.contains("retention_days") && ins["retention_days"].is_number_integer()) insights.retentionDays = ins["retention_days"];
        if (ins.contains("refresh_interval_sec") && ins["refresh_interval_sec"].is_number_integer()) insights.refreshIntervalSec = ins["refresh_interval_sec"];
        if (ins.contains("long_context_threshold_tokens") && ins["long_context_threshold_tokens"].is_number_integer()) insights.longContextThresholdTokens = ins["long_context_threshold_tokens"];
        if (ins.contains("compact_context_threshold_tokens") && ins["compact_context_threshold_tokens"].is_number_integer()) insights.compactContextThresholdTokens = ins["compact_context_threshold_tokens"];
        if (ins.contains("compact_continue_requests") && ins["compact_continue_requests"].is_number_integer()) insights.compactContinueRequests = ins["compact_continue_requests"];
        if (ins.contains("subagent_share_threshold") && ins["subagent_share_threshold"].is_number()) insights.subagentShareThreshold = ins["subagent_share_threshold"];
        if (ins.contains("long_session_hours") && ins["long_session_hours"].is_number_integer()) insights.longSessionHours = ins["long_session_hours"];
        if (ins.contains("cache_efficiency_threshold") && ins["cache_efficiency_threshold"].is_number()) insights.cacheEfficiencyThreshold = ins["cache_efficiency_threshold"];
        if (ins.contains("cache_efficiency_min_requests") && ins["cache_efficiency_min_requests"].is_number_integer()) insights.cacheEfficiencyMinRequests = ins["cache_efficiency_min_requests"];
        if (ins.contains("error_retry_streak_threshold") && ins["error_retry_streak_threshold"].is_number_integer()) insights.errorRetryStreakThreshold = ins["error_retry_streak_threshold"];
        if (ins.contains("idle_gap_seconds") && ins["idle_gap_seconds"].is_number_integer()) insights.idleGapSeconds = ins["idle_gap_seconds"];
        if (ins.contains("burn_rate_alert_fraction") && ins["burn_rate_alert_fraction"].is_number()) insights.burnRateAlertFraction = ins["burn_rate_alert_fraction"];
        if (ins.contains("short_prompt_max_prose_chars") && ins["short_prompt_max_prose_chars"].is_number_integer()) insights.shortPromptMaxProseChars = ins["short_prompt_max_prose_chars"];
        if (ins.contains("drip_prompt_max_prose_chars") && ins["drip_prompt_max_prose_chars"].is_number_integer()) insights.dripPromptMaxProseChars = ins["drip_prompt_max_prose_chars"];
        if (ins.contains("drip_prompts_per_hour") && ins["drip_prompts_per_hour"].is_number_integer()) insights.dripPromptsPerHour = ins["drip_prompts_per_hour"];
        if (ins.contains("quick_reprompt_seconds") && ins["quick_reprompt_seconds"].is_number_integer()) insights.quickRepromptSeconds = ins["quick_reprompt_seconds"];
        if (ins.contains("coaching_min_sample_size") && ins["coaching_min_sample_size"].is_number_integer()) insights.coachingMinSampleSize = ins["coaching_min_sample_size"];
    }

    if (j.contains("eda") && j["eda"].is_object()) {
        eda.fromJson(j["eda"]);
    }
}

bool AimonConfig::validate(std::string& error) const {
    if (polling.baseIntervalSec < 10 || polling.baseIntervalSec > 86400) {
        error = "polling.base_interval_sec must be between 10 and 86400";
        return false;
    }
    if (polling.idleIntervalSec < 10 || polling.idleIntervalSec > 86400) {
        error = "polling.idle_interval_sec must be between 10 and 86400";
        return false;
    }
    if (polling.activeIntervalSec < 5 || polling.activeIntervalSec > 86400) {
        error = "polling.active_interval_sec must be between 5 and 86400";
        return false;
    }
    if (web.port < 1 || web.port > 65535) {
        error = "web.port must be between 1 and 65535";
        return false;
    }
    if (mqtt.port < 1 || mqtt.port > 65535) {
        error = "mqtt.port must be between 1 and 65535";
        return false;
    }
    if (!claude.validate(error)) return false;
    if (!gateway.validate(error)) return false;
    if (!eda.validate(error)) return false;
    error.clear();
    return true;
}

std::vector<ClaudeAccountConfig> ClaudeConfig::resolvedAccounts() const {
    if (!accounts.empty()) {
        return accounts;
    }
    ClaudeAccountConfig def;
    def.name = "default";
    const char* env = std::getenv("CLAUDE_CONFIG_DIR");
    def.configDir = (env && *env) ? env : "~/.claude";
    return {def};
}

bool ClaudeConfig::validate(std::string& error) const {
    if (windowDays < 1 || windowDays > 365) {
        error = "claude.window_days must be between 1 and 365";
        return false;
    }
    if (retentionDays < 30 || retentionDays > 3650) {
        error = "claude.retention_days must be between 30 and 3650";
        return false;
    }
    if (pricingRefreshHours < 1 || pricingRefreshHours > 168) {
        error = "claude.pricing_refresh_hours must be between 1 and 168";
        return false;
    }
    if (!pricingUrl.empty() && pricingUrl.rfind("https://", 0) != 0) {
        error = "claude.pricing_url must start with https://";
        return false;
    }
    std::vector<std::string> names;
    std::vector<std::string> dirs;
    for (const auto& a : resolvedAccounts()) {
        if (a.name.empty() || a.name.size() > 64 ||
            a.name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-") != std::string::npos) {
            error = "claude account names must be 1-64 characters of letters, digits, '_', '.' or '-'";
            return false;
        }
        if (a.configDir.empty()) {
            error = "claude account '" + a.name + "' has no config_dir";
            return false;
        }
        if (!(a.spendLimitUsd >= 0.0) || a.spendLimitUsd > 1e9) {
            error = "claude account '" + a.name + "' has an invalid spend_limit_usd";
            return false;
        }
        if (a.cycleResetDay != 0 && (a.cycleResetDay < 1 || a.cycleResetDay > 28)) {
            error = "claude account '" + a.name + "' has cycle_reset_day outside 1..28";
            return false;
        }
        std::string dir = fs::path(PathUtils::expandHome(a.configDir)).lexically_normal().string();
        while (dir.size() > 1 && dir.back() == '/') {
            dir.pop_back();
        }
        for (const auto& n : names) {
            if (n == a.name) {
                error = "duplicate claude account name '" + a.name + "'";
                return false;
            }
        }
        for (const auto& d : dirs) {
            if (d == dir) {
                error = "claude accounts must not share a config_dir ('" + a.name + "')";
                return false;
            }
        }
        names.push_back(a.name);
        dirs.push_back(dir);
    }
    return true;
}

ConfigManager& ConfigManager::getInstance() {
    static ConfigManager instance;
    return instance;
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

        if (root.exists("claude")) {
            const libconfig::Setting& cl = root["claude"];
            readSetting(cl, "enabled", _config.claude.enabled);
            readSetting(cl, "window_days", _config.claude.windowDays);
            readSetting(cl, "retention_days", _config.claude.retentionDays);
            readSetting(cl, "pricing_url", _config.claude.pricingUrl);
            readSetting(cl, "pricing_refresh_hours", _config.claude.pricingRefreshHours);
            if (cl.exists("accounts")) {
                const libconfig::Setting& aList = cl["accounts"];
                if (aList.isList() || aList.isArray()) {
                    _config.claude.accounts.clear();
                    for (int i = 0; i < aList.getLength(); ++i) {
                        const libconfig::Setting& aElem = aList[i];
                        ClaudeAccountConfig acct;
                        readSetting(aElem, "name", acct.name);
                        readSetting(aElem, "config_dir", acct.configDir);
                        readSetting(aElem, "spend_limit_usd", acct.spendLimitUsd);
                        readSetting(aElem, "cycle_reset_day", acct.cycleResetDay);
                        _config.claude.accounts.push_back(acct);
                    }
                }
            }
        }

        bool hasGwRetention = false;
        if (root.exists("gateway")) {
            const libconfig::Setting& gw = root["gateway"];
            readSetting(gw, "enabled", _config.gateway.enabled);
            readSetting(gw, "host", _config.gateway.host);
            readSetting(gw, "port", _config.gateway.port);
            readSetting(gw, "allow_cli_new_session_spawn", _config.gateway.allowCliNewSessionSpawn);
            readSetting(gw, "allow_agy_agentapi_fallback", _config.gateway.allowAgyAgentApiFallback);
            readSetting(gw, "strict_presence_fd_check", _config.gateway.strictPresenceFdCheck);
            if (readSetting(gw, "prune_stale_presence_locks", _config.gateway.pruneStalePresenceLocks)) {
                _config.gateway.autoPruneStalePresenceLocks = _config.gateway.pruneStalePresenceLocks;
            }
            if (readSetting(gw, "auto_prune_stale_presence_locks", _config.gateway.autoPruneStalePresenceLocks)) {
                _config.gateway.pruneStalePresenceLocks = _config.gateway.autoPruneStalePresenceLocks;
            }
            if (readSetting(gw, "stale_presence_lock_max_age_sec", _config.gateway.stalePresenceLockMaxAgeSec)) {
                _config.gateway.presenceStaleTtlSec = _config.gateway.stalePresenceLockMaxAgeSec;
            }
            if (readSetting(gw, "presence_stale_ttl_sec", _config.gateway.presenceStaleTtlSec)) {
                _config.gateway.stalePresenceLockMaxAgeSec = _config.gateway.presenceStaleTtlSec;
            }
            readSetting(gw, "active_recency_window_sec", _config.gateway.activeRecencyWindowSec);
            hasGwRetention = readSetting(gw, "telemetry_retention_days", _config.gateway.telemetryRetentionDays);
        }

        if (!hasGwRetention && root.exists("diagnostics")) {
            const libconfig::Setting& dg = root["diagnostics"];
            readSetting(dg, "telemetry_retention_days", _config.gateway.telemetryRetentionDays);
        }

        if (root.exists("insights")) {
            const libconfig::Setting& ins = root["insights"];
            readSetting(ins, "enabled", _config.insights.enabled);
            readSetting(ins, "db_path", _config.insights.dbPath);
            readSetting(ins, "retention_days", _config.insights.retentionDays);
            readSetting(ins, "refresh_interval_sec", _config.insights.refreshIntervalSec);
            readSetting(ins, "long_context_threshold_tokens", _config.insights.longContextThresholdTokens);
            readSetting(ins, "compact_context_threshold_tokens", _config.insights.compactContextThresholdTokens);
            readSetting(ins, "compact_continue_requests", _config.insights.compactContinueRequests);
            readSetting(ins, "subagent_share_threshold", _config.insights.subagentShareThreshold);
            readSetting(ins, "long_session_hours", _config.insights.longSessionHours);
            readSetting(ins, "cache_efficiency_threshold", _config.insights.cacheEfficiencyThreshold);
            readSetting(ins, "cache_efficiency_min_requests", _config.insights.cacheEfficiencyMinRequests);
            readSetting(ins, "error_retry_streak_threshold", _config.insights.errorRetryStreakThreshold);
            readSetting(ins, "idle_gap_seconds", _config.insights.idleGapSeconds);
            readSetting(ins, "burn_rate_alert_fraction", _config.insights.burnRateAlertFraction);
            readSetting(ins, "short_prompt_max_prose_chars", _config.insights.shortPromptMaxProseChars);
            readSetting(ins, "drip_prompt_max_prose_chars", _config.insights.dripPromptMaxProseChars);
            readSetting(ins, "drip_prompts_per_hour", _config.insights.dripPromptsPerHour);
            readSetting(ins, "quick_reprompt_seconds", _config.insights.quickRepromptSeconds);
            readSetting(ins, "coaching_min_sample_size", _config.insights.coachingMinSampleSize);
        }

        if (root.exists("eda")) {
            const libconfig::Setting& eda = root["eda"];
            readSetting(eda, "enabled", _config.eda.enabled);
            readSetting(eda, "mode", _config.eda.mode);
            readSetting(eda, "host", _config.eda.host);
            readSetting(eda, "port", _config.eda.port);
            readSetting(eda, "auth_secret", _config.eda.authSecret);
            readSetting(eda, "health_poll_interval_sec", _config.eda.healthPollIntervalSec);
            readSetting(eda, "timeout_ms", _config.eda.timeoutMs);
            readSetting(eda, "max_output_bytes", _config.eda.maxOutputBytes);

            if (eda.exists("accelerators")) {
                const libconfig::Setting& acc = eda["accelerators"];
                readSetting(acc, "enabled", _config.eda.accelerators.enabled);
                readSetting(acc, "sa_placement_iterations", _config.eda.accelerators.saPlacementIterations);
                readSetting(acc, "autoroute_max_ripup_passes", _config.eda.accelerators.autorouteMaxRipupPasses);
                readSetting(acc, "autoroute_grid_mm", _config.eda.accelerators.autorouteGridMm);
                readSetting(acc, "prefer_jlcpcb_basic_e_series", _config.eda.accelerators.preferJlcpcbBasicESeries);
                readSetting(acc, "default_copper_weight_oz", _config.eda.accelerators.defaultCopperWeightOz);
                readSetting(acc, "default_filament_material", _config.eda.accelerators.defaultFilamentMaterial);
                readSetting(acc, "default_gear_pressure_angle_deg", _config.eda.accelerators.defaultGearPressureAngleDeg);
            }

            if (eda.exists("easyeda")) {
                const libconfig::Setting& ee = eda["easyeda"];
                readSetting(ee, "enabled", _config.eda.easyeda.enabled);
            }

            if (eda.exists("kicad")) {
                const libconfig::Setting& kc = eda["kicad"];
                readSetting(kc, "enabled", _config.eda.kicad.enabled);
            }

            if (eda.exists("freecad")) {
                const libconfig::Setting& fc = eda["freecad"];
                readSetting(fc, "enabled", _config.eda.freecad.enabled);
            }
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

        // claude
        libconfig::Setting& cl = root.add("claude", libconfig::Setting::TypeGroup);
        cl.add("enabled", libconfig::Setting::TypeBoolean) = _config.claude.enabled;
        cl.add("window_days", libconfig::Setting::TypeInt) = _config.claude.windowDays;
        cl.add("retention_days", libconfig::Setting::TypeInt) = _config.claude.retentionDays;
        cl.add("pricing_url", libconfig::Setting::TypeString) = _config.claude.pricingUrl;
        cl.add("pricing_refresh_hours", libconfig::Setting::TypeInt) = _config.claude.pricingRefreshHours;
        libconfig::Setting& aList = cl.add("accounts", libconfig::Setting::TypeList);
        for (const auto& acct : _config.claude.accounts) {
            libconfig::Setting& aElem = aList.add(libconfig::Setting::TypeGroup);
            aElem.add("name", libconfig::Setting::TypeString) = acct.name;
            aElem.add("config_dir", libconfig::Setting::TypeString) = acct.configDir;
            aElem.add("spend_limit_usd", libconfig::Setting::TypeFloat) = acct.spendLimitUsd;
            aElem.add("cycle_reset_day", libconfig::Setting::TypeInt) = acct.cycleResetDay;
        }

        // gateway
        libconfig::Setting& gw = root.add("gateway", libconfig::Setting::TypeGroup);
        gw.add("enabled", libconfig::Setting::TypeBoolean) = _config.gateway.enabled;
        gw.add("host", libconfig::Setting::TypeString) = _config.gateway.host;
        gw.add("port", libconfig::Setting::TypeInt) = _config.gateway.port;
        gw.add("allow_cli_new_session_spawn", libconfig::Setting::TypeBoolean) =
            _config.gateway.allowCliNewSessionSpawn;
        gw.add("allow_agy_agentapi_fallback", libconfig::Setting::TypeBoolean) =
            _config.gateway.allowAgyAgentApiFallback;
        gw.add("strict_presence_fd_check", libconfig::Setting::TypeBoolean) =
            _config.gateway.strictPresenceFdCheck;
        gw.add("auto_prune_stale_presence_locks", libconfig::Setting::TypeBoolean) =
            _config.gateway.autoPruneStalePresenceLocks;
        gw.add("prune_stale_presence_locks", libconfig::Setting::TypeBoolean) =
            _config.gateway.autoPruneStalePresenceLocks;
        gw.add("presence_stale_ttl_sec", libconfig::Setting::TypeInt) =
            _config.gateway.presenceStaleTtlSec;
        gw.add("stale_presence_lock_max_age_sec", libconfig::Setting::TypeInt) =
            _config.gateway.presenceStaleTtlSec;
        gw.add("active_recency_window_sec", libconfig::Setting::TypeInt) =
            _config.gateway.activeRecencyWindowSec;
        gw.add("telemetry_retention_days", libconfig::Setting::TypeInt) =
            _config.gateway.telemetryRetentionDays;

        // insights
        libconfig::Setting& ins = root.add("insights", libconfig::Setting::TypeGroup);
        ins.add("enabled", libconfig::Setting::TypeBoolean) = _config.insights.enabled;
        ins.add("db_path", libconfig::Setting::TypeString) = _config.insights.dbPath;
        ins.add("retention_days", libconfig::Setting::TypeInt) = _config.insights.retentionDays;
        ins.add("refresh_interval_sec", libconfig::Setting::TypeInt) = _config.insights.refreshIntervalSec;
        ins.add("long_context_threshold_tokens", libconfig::Setting::TypeInt) = _config.insights.longContextThresholdTokens;
        ins.add("compact_context_threshold_tokens", libconfig::Setting::TypeInt) = _config.insights.compactContextThresholdTokens;
        ins.add("compact_continue_requests", libconfig::Setting::TypeInt) = _config.insights.compactContinueRequests;
        ins.add("subagent_share_threshold", libconfig::Setting::TypeFloat) = _config.insights.subagentShareThreshold;
        ins.add("long_session_hours", libconfig::Setting::TypeInt) = _config.insights.longSessionHours;
        ins.add("cache_efficiency_threshold", libconfig::Setting::TypeFloat) = _config.insights.cacheEfficiencyThreshold;
        ins.add("cache_efficiency_min_requests", libconfig::Setting::TypeInt) = _config.insights.cacheEfficiencyMinRequests;
        ins.add("error_retry_streak_threshold", libconfig::Setting::TypeInt) = _config.insights.errorRetryStreakThreshold;
        ins.add("idle_gap_seconds", libconfig::Setting::TypeInt) = _config.insights.idleGapSeconds;
        ins.add("burn_rate_alert_fraction", libconfig::Setting::TypeFloat) = _config.insights.burnRateAlertFraction;
        ins.add("short_prompt_max_prose_chars", libconfig::Setting::TypeInt) = _config.insights.shortPromptMaxProseChars;
        ins.add("drip_prompt_max_prose_chars", libconfig::Setting::TypeInt) = _config.insights.dripPromptMaxProseChars;
        ins.add("drip_prompts_per_hour", libconfig::Setting::TypeInt) = _config.insights.dripPromptsPerHour;
        ins.add("quick_reprompt_seconds", libconfig::Setting::TypeInt) = _config.insights.quickRepromptSeconds;
        ins.add("coaching_min_sample_size", libconfig::Setting::TypeInt) = _config.insights.coachingMinSampleSize;

        // eda
        libconfig::Setting& eda = root.add("eda", libconfig::Setting::TypeGroup);
        eda.add("enabled", libconfig::Setting::TypeBoolean) = _config.eda.enabled;
        eda.add("mode", libconfig::Setting::TypeString) = _config.eda.mode;
        eda.add("host", libconfig::Setting::TypeString) = _config.eda.host;
        eda.add("port", libconfig::Setting::TypeInt) = _config.eda.port;
        eda.add("auth_secret", libconfig::Setting::TypeString) = _config.eda.authSecret;
        eda.add("health_poll_interval_sec", libconfig::Setting::TypeInt) = _config.eda.healthPollIntervalSec;
        eda.add("timeout_ms", libconfig::Setting::TypeInt) = _config.eda.timeoutMs;
        eda.add("max_output_bytes", libconfig::Setting::TypeInt) = _config.eda.maxOutputBytes;

        libconfig::Setting& acc = eda.add("accelerators", libconfig::Setting::TypeGroup);
        acc.add("enabled", libconfig::Setting::TypeBoolean) = _config.eda.accelerators.enabled;
        acc.add("sa_placement_iterations", libconfig::Setting::TypeInt) = _config.eda.accelerators.saPlacementIterations;
        acc.add("autoroute_max_ripup_passes", libconfig::Setting::TypeInt) = _config.eda.accelerators.autorouteMaxRipupPasses;
        acc.add("autoroute_grid_mm", libconfig::Setting::TypeFloat) = _config.eda.accelerators.autorouteGridMm;
        acc.add("prefer_jlcpcb_basic_e_series", libconfig::Setting::TypeBoolean) = _config.eda.accelerators.preferJlcpcbBasicESeries;
        acc.add("default_copper_weight_oz", libconfig::Setting::TypeFloat) = _config.eda.accelerators.defaultCopperWeightOz;
        acc.add("default_filament_material", libconfig::Setting::TypeString) = _config.eda.accelerators.defaultFilamentMaterial;
        acc.add("default_gear_pressure_angle_deg", libconfig::Setting::TypeFloat) = _config.eda.accelerators.defaultGearPressureAngleDeg;

        libconfig::Setting& ee = eda.add("easyeda", libconfig::Setting::TypeGroup);
        ee.add("enabled", libconfig::Setting::TypeBoolean) = _config.eda.easyeda.enabled;

        libconfig::Setting& kc = eda.add("kicad", libconfig::Setting::TypeGroup);
        kc.add("enabled", libconfig::Setting::TypeBoolean) = _config.eda.kicad.enabled;

        libconfig::Setting& fc = eda.add("freecad", libconfig::Setting::TypeGroup);
        fc.add("enabled", libconfig::Setting::TypeBoolean) = _config.eda.freecad.enabled;

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

    const char* edaSecret = std::getenv("AIMON_EDA_AUTH_SECRET");
    if (edaSecret && *edaSecret) {
        _config.eda.authSecret = edaSecret;
    }

    const char* edaHost = std::getenv("AIMON_EDA_HOST");
    if (edaHost && *edaHost) {
        _config.eda.host = edaHost;
    }

    const char* edaPort = std::getenv("AIMON_EDA_PORT");
    if (edaPort && *edaPort) {
        int parsed = std::atoi(edaPort);
        if (parsed >= 1 && parsed <= 65535) {
            _config.eda.port = parsed;
        }
    }
}

AimonConfig ConfigManager::getConfigSnapshot() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _config;
}

bool ConfigManager::updateFromJson(const nlohmann::json& patch, std::string& error) {
    std::lock_guard<std::mutex> lock(_mutex);
    AimonConfig candidate = _config;
    candidate.fromJson(patch);
    if (!candidate.validate(error)) {
        _lastErrorMessage = error;
        return false;
    }
    _config = candidate;
    if (!save()) {
        error = "Failed to persist configuration to " + _configFilePath;
        _lastErrorMessage = error;
        return false;
    }
    error.clear();
    return true;
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
