/*
 * MqttPublisher.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "MqttPublisher.hxx"
#include <iostream>
#include <mosquitto.h>
#include <algorithm>
#include <nlohmann/json.hpp>

namespace aimon {

static std::string slugify(const std::string& input) {
    std::string out;
    for (char c : input) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out.push_back(std::tolower(static_cast<unsigned char>(c)));
        } else if (c == '-' || c == '_' || c == ' ') {
            if (!out.empty() && out.back() != '_') {
                out.push_back('_');
            }
        }
    }
    while (!out.empty() && out.back() == '_') {
        out.pop_back();
    }
    return out;
}

namespace {

std::string sensorDiscoveryPayload(const std::string& sensorId, const std::string& sensorName,
                                   const std::string& stateTopic, const std::string& unit,
                                   const std::string& icon, const std::string& jsonAttributesTopic) {
    nlohmann::json payload = {
        {"name", sensorName},
        {"state_topic", stateTopic},
        {"unique_id", "aimon_" + sensorId},
        {"device", {
            {"identifiers", nlohmann::json::array({"aimon_quota_monitor"})},
            {"name", "AI Quota Monitor"},
            {"model", "aimon v1.0"},
            {"manufacturer", "aimon"}
        }}
    };
    if (!unit.empty()) {
        payload["unit_of_measurement"] = unit;
    }
    if (!icon.empty()) {
        payload["icon"] = icon;
    }
    if (!jsonAttributesTopic.empty()) {
        payload["json_attributes_topic"] = jsonAttributesTopic;
    }
    return payload.dump();
}

// One topic-safe, unique slug per account (names are validated elsewhere, but
// this must hold for any name).
std::vector<std::string> claudeSlugs(const ClaudeStatus& status) {
    std::set<std::string> used;
    std::vector<std::string> slugs;
    for (const auto& a : status.accounts) {
        std::string base = slugify(a.name);
        if (base.empty()) {
            base = "account";
        }
        std::string candidate = base;
        for (int n = 2; used.count(candidate); ++n) {
            candidate = base + "_" + std::to_string(n);
        }
        used.insert(candidate);
        slugs.push_back(candidate);
    }
    return slugs;
}

std::string usd2(int64_t nano) {
    if (nano < 0) {
        nano = 0;
    }
    const int64_t cents = (nano + 5000000) / 10000000;
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%lld.%02lld", static_cast<long long>(cents / 100),
                  static_cast<long long>(cents % 100));
    return buf;
}

bool claudeReports(const ClaudeAccountStatus& a) {
    return a.hasData && !a.window.costOverflow;
}

bool claudeHasLimit(const ClaudeAccountStatus& a) {
    return a.cycleConfigured && a.spendLimitUsd > 0.0 && !a.cycle.costOverflow;
}

} // namespace

std::vector<MqttMessage> MqttPublisher::buildClaudeDiscovery(const std::string& topicPrefix,
                                                             const std::string& discoveryPrefix,
                                                             const ClaudeStatus& status) {
    std::vector<MqttMessage> out;
    if (!status.enabled) {
        return out;
    }
    const std::vector<std::string> slugs = claudeSlugs(status);
    for (size_t i = 0; i < status.accounts.size(); ++i) {
        const ClaudeAccountStatus& a = status.accounts[i];
        if (!claudeReports(a)) {
            continue;
        }
        const std::string slug = slugs[i];
        const std::string base = topicPrefix + "/claude/" + slug;
        const std::string label = "Claude " + a.name;
        auto add = [&](const std::string& metric, const std::string& name, const std::string& unit,
                       const std::string& icon, const std::string& attrs) {
            const std::string sensorId = "claude_" + slug + "_" + metric;
            out.push_back({discoveryPrefix + "/sensor/aimon/" + sensorId + "/config",
                           sensorDiscoveryPayload(sensorId, name, base + "/" + metric + "/state", unit, icon, attrs)});
        };
        add("est_cost_window_usd", label + " Est. Cost (" + std::to_string(a.windowDays) + " d)", "$",
            "mdi:currency-usd", base + "/attributes");
        if (a.cycleConfigured) {
            add("est_cost_cycle_usd", label + " Est. Cost (billing cycle)", "$", "mdi:calendar-month", "");
        }
        if (claudeHasLimit(a)) {
            add("est_pct_of_limit", label + " Est. Share of Spend Limit", "%", "mdi:gauge", "");
        }
        add("tokens_5h", label + " Tokens (5 h)", "tokens", "mdi:chart-line", "");
        add("tokens_7d", label + " Tokens (7 d)", "tokens", "mdi:chart-line", "");
        add("tokens_window", label + " Tokens (" + std::to_string(a.windowDays) + " d)", "tokens",
            "mdi:chart-line", "");
        add("tier", label + " Plan Tier", "", "mdi:account-star", "");
    }
    return out;
}

std::vector<MqttMessage> MqttPublisher::buildClaudeState(const std::string& topicPrefix, const ClaudeStatus& status) {
    std::vector<MqttMessage> out;
    if (!status.enabled) {
        return out;
    }
    const std::vector<std::string> slugs = claudeSlugs(status);
    for (size_t i = 0; i < status.accounts.size(); ++i) {
        const ClaudeAccountStatus& a = status.accounts[i];
        if (!claudeReports(a)) {
            continue;
        }
        const std::string base = topicPrefix + "/claude/" + slugs[i];
        out.push_back({base + "/est_cost_window_usd/state", usd2(a.window.costNano)});
        if (a.cycleConfigured) {
            out.push_back({base + "/est_cost_cycle_usd/state", usd2(a.cycle.costNano)});
        }
        if (claudeHasLimit(a)) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.1f", a.estPctOfLimit);
            out.push_back({base + "/est_pct_of_limit/state", buf});
        }
        out.push_back({base + "/tokens_5h/state", std::to_string(a.last5h.totalTokens())});
        out.push_back({base + "/tokens_7d/state", std::to_string(a.last7d.totalTokens())});
        out.push_back({base + "/tokens_window/state", std::to_string(a.window.totalTokens())});
        out.push_back({base + "/tier/state", claudeTierName(a.tier)});
        nlohmann::json attrs = {
            {"name", a.name},
            {"tier", claudeTierName(a.tier)},
            {"raw_subscription_type", a.rawSubscriptionType},
            {"raw_rate_limit_tier", a.rawRateLimitTier},
            {"prices_stale", status.pricing.stale},
            {"prices_as_of_epoch", status.pricing.fetchedAtEpoch},
            {"unpriced_models", a.unpricedModels},
            {"warning", a.warning},
            {"error", a.errorMessage}
        };
        out.push_back({base + "/attributes", attrs.dump()});
    }
    return out;
}

MqttPublisher::MqttPublisher(const MqttConfig& config)
    : _config(config) {
}

MqttPublisher::~MqttPublisher() {
    stop();
}

bool MqttPublisher::start() {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_connected && _mosq) {
        return true;
    }

    mosquitto_lib_init();
    _mosq = mosquitto_new("aimon_monitor", true, this);
    if (!_mosq) {
        std::cerr << "[MqttPublisher] Failed to create mosquitto client instance" << std::endl;
        return false;
    }

    if (!_config.username.empty()) {
        mosquitto_username_pw_set(_mosq, _config.username.c_str(),
                                  _config.password.empty() ? nullptr : _config.password.c_str());
    }

    int rc = mosquitto_connect(_mosq, _config.broker.c_str(), _config.port, 60);
    if (rc != MOSQ_ERR_SUCCESS) {
        std::cerr << "[MqttPublisher] Warning: could not connect to MQTT broker "
                  << _config.broker << ":" << _config.port << " ("
                  << mosquitto_strerror(rc) << ")" << std::endl;
        _connected = false;
        return false;
    }

    mosquitto_loop_start(_mosq);
    _connected = true;

    publishDiscovery();
    return true;
}

void MqttPublisher::stop() {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_mosq) {
        mosquitto_disconnect(_mosq);
        mosquitto_loop_stop(_mosq, false);
        mosquitto_destroy(_mosq);
        _mosq = nullptr;
    }
    mosquitto_lib_cleanup();
    _connected = false;
}

bool MqttPublisher::publishMessage(const std::string& topic, const std::string& payload, bool retain) {
    if (!_connected || !_mosq) {
        return false;
    }

    int rc = mosquitto_publish(_mosq, nullptr, topic.c_str(),
                               static_cast<int>(payload.length()), payload.c_str(),
                               0, retain);
    return (rc == MOSQ_ERR_SUCCESS);
}

void MqttPublisher::publishSensorDiscovery(const std::string& sensorId,
                                           const std::string& sensorName,
                                           const std::string& stateTopic,
                                           const std::string& unit,
                                           const std::string& icon,
                                           const std::string& jsonAttributesTopic) {
    std::string discoveryTopic = _config.discoveryPrefix + "/sensor/aimon/" + sensorId + "/config";
    publishMessage(discoveryTopic,
                   sensorDiscoveryPayload(sensorId, sensorName, stateTopic, unit, icon, jsonAttributesTopic),
                   _config.retain);
}

void MqttPublisher::publishDiscovery() {
    std::string prefix = _config.topicPrefix;

    publishSensorDiscovery("ag_plan_tier", "Antigravity Plan Tier",
                           prefix + "/antigravity/plan_tier/state", "", "mdi:account-badge");

    publishSensorDiscovery("cursor_fast_requests_used", "Cursor Fast Requests Used",
                           prefix + "/cursor/fast_requests_used/state", "reqs", "mdi:chart-line");
    publishSensorDiscovery("cursor_fast_requests_limit", "Cursor Fast Requests Limit",
                           prefix + "/cursor/fast_requests_limit/state", "reqs", "mdi:gauge");
    publishSensorDiscovery("cursor_plan_tier", "Cursor Plan Tier",
                           prefix + "/cursor/plan_tier/state", "", "mdi:account-star");
    publishSensorDiscovery("cursor_cycle_reset", "Cursor Cycle Reset",
                           prefix + "/cursor/cycle_reset/state", "", "mdi:calendar-clock");
    publishSensorDiscovery("cursor_cycle_spend", "Cursor Current Cycle Spend",
                           prefix + "/cursor/cycle_spend/state", "$", "mdi:currency-usd");
    publishSensorDiscovery("cursor_prev_cycle_spend", "Cursor Previous Cycle Spend",
                           prefix + "/cursor/prev_cycle_spend/state", "$", "mdi:history");
}

void MqttPublisher::publishState(const AggregateStatus& status) {
    if (!_connected) {
        start();
        if (!_connected) return;
    }

    std::string prefix = _config.topicPrefix;

    if (status.antigravity.isRunning) {
        publishMessage(prefix + "/antigravity/plan_tier/state",
                       status.antigravity.planTier, _config.retain);

        // Publish quota groups & limit buckets
        for (const auto& group : status.antigravity.quotaGroups) {
            for (const auto& b : group.buckets) {
                std::string slug = slugify(b.bucketId.empty() ? (group.displayName + "_" + b.displayName) : b.bucketId);
                std::string sensorId = "ag_quota_" + slug;

                if (_discoveredModels.find(sensorId) == _discoveredModels.end()) {
                    std::string niceName = group.displayName + " " + b.displayName;
                    publishSensorDiscovery(sensorId, niceName,
                                           prefix + "/antigravity/quota_" + slug + "/state",
                                           "%", "mdi:gauge",
                                           prefix + "/antigravity/quota_" + slug + "/attributes");
                    _discoveredModels.insert(sensorId);
                }

                int percent = static_cast<int>(b.remainingFraction * 100.0 + 0.5);
                publishMessage(prefix + "/antigravity/quota_" + slug + "/state",
                               std::to_string(percent), _config.retain);

                nlohmann::json attrs = {
                    {"reset_time_iso", b.resetTimeIso},
                    {"group_name", group.displayName},
                    {"bucket_id", b.bucketId},
                    {"window", b.window},
                    {"description", b.description}
                };
                publishMessage(prefix + "/antigravity/quota_" + slug + "/attributes",
                               attrs.dump(), _config.retain);
            }
        }

        for (const auto& m : status.antigravity.models) {
            std::string slug = slugify(m.modelId.empty() ? m.modelName : m.modelId);
            std::string sensorId = "ag_model_" + slug;

            if (_discoveredModels.find(sensorId) == _discoveredModels.end()) {
                publishSensorDiscovery(sensorId, m.modelName + " Capacity",
                                       prefix + "/antigravity/model_" + slug + "/state",
                                       "%", "mdi:speedometer",
                                       prefix + "/antigravity/model_" + slug + "/attributes");
                _discoveredModels.insert(sensorId);
            }

            int percent = static_cast<int>(m.remainingFraction * 100.0 + 0.5);
            publishMessage(prefix + "/antigravity/model_" + slug + "/state",
                           std::to_string(percent), _config.retain);

            nlohmann::json attrs = {
                {"reset_time_iso", m.resetTimeIso},
                {"model_name", m.modelName},
                {"model_id", m.modelId}
            };
            publishMessage(prefix + "/antigravity/model_" + slug + "/attributes",
                           attrs.dump(), _config.retain);
        }
    }

    if (status.cursor.isAuthenticated) {
        publishMessage(prefix + "/cursor/fast_requests_used/state",
                       std::to_string(status.cursor.fastRequestsUsed), _config.retain);
        publishMessage(prefix + "/cursor/fast_requests_limit/state",
                       std::to_string(status.cursor.fastRequestsLimit), _config.retain);
        publishMessage(prefix + "/cursor/plan_tier/state",
                       status.cursor.planTier, _config.retain);
        publishMessage(prefix + "/cursor/cycle_reset/state",
                       status.cursor.cycleResetIso, _config.retain);

        char spendBuf[32];
        std::snprintf(spendBuf, sizeof(spendBuf), "%.2f", status.cursor.totalSpendUsd);
        publishMessage(prefix + "/cursor/cycle_spend/state", spendBuf, _config.retain);

        std::snprintf(spendBuf, sizeof(spendBuf), "%.2f", status.cursor.prevCycleSpendUsd);
        publishMessage(prefix + "/cursor/prev_cycle_spend/state", spendBuf, _config.retain);
    }

    for (const MqttMessage& d : buildClaudeDiscovery(prefix, _config.discoveryPrefix, status.claude)) {
        if (_discoveredModels.insert(d.topic).second) {
            publishMessage(d.topic, d.payload, _config.retain);
        }
    }
    for (const MqttMessage& m : buildClaudeState(prefix, status.claude)) {
        publishMessage(m.topic, m.payload, _config.retain);
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
