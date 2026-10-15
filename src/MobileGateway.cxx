/*
 * MobileGateway.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "MobileGateway.hxx"
#include "AgentTelemetryDb.hxx"
#include "PathUtils.hxx"
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <sys/stat.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace aimon {

MobileGateway &MobileGateway::getInstance() {
    static MobileGateway instance;
    return instance;
}

MobileGateway::MobileGateway()
    : _storagePath("")
    , _lastGeneratedSecret("")
    , _lastGeneratedExpires(0)
    , _broadcastCb(nullptr) {
}

MobileGateway::~MobileGateway() {
    shutdown();
}

bool MobileGateway::init(const std::string &storagePath) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (storagePath.empty()) {
        _storagePath = PathUtils::getAimonConfigDir() + "/mobile_devices.json";
    } else {
        _storagePath = PathUtils::expandHome(storagePath);
    }

    fs::path p(_storagePath);
    if (p.has_parent_path() && !p.parent_path().empty()) {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        chmod(p.parent_path().c_str(), 0700);
    }

    _sessions.clear();
    return loadSessionsLocked();
}

bool MobileGateway::loadSessions() {
    std::lock_guard<std::mutex> lock(_mutex);
    return loadSessionsLocked();
}

bool MobileGateway::loadSessionsLocked() {
    if (_storagePath.empty() || !fs::exists(_storagePath)) {
        return true;
    }

    std::ifstream in(_storagePath, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }

    json root = json::parse(in, nullptr, false);
    if (root.is_discarded()) {
        return false;
    }

    const json *devicesPtr = nullptr;
    if (root.is_object() && root.contains("devices") && root["devices"].is_array()) {
        devicesPtr = &root["devices"];
    } else if (root.is_array()) {
        devicesPtr = &root;
    } else {
        return false;
    }

    _sessions.clear();
    for (const auto &item : *devicesPtr) {
        if (!item.is_object()) continue;
        MobileSession s;
        s.deviceId = item.value("device_id", "");
        s.deviceName = item.value("device_name", "Android Client");
        s.token = item.value("token", "");
        s.pairedAt = static_cast<time_t>(item.value("paired_at", int64_t(0)));
        s.lastSeenAt = static_cast<time_t>(item.value("last_seen_at", int64_t(0)));
        s.isRevoked = item.value("is_revoked", false);

        if (!s.token.empty() && !s.isRevoked) {
            _sessions[s.token] = s;
        }
    }
    return true;
}

bool MobileGateway::saveSessions() {
    std::lock_guard<std::mutex> lock(_mutex);
    return saveSessionsLocked();
}

bool MobileGateway::saveSessionsLocked() {
    if (_storagePath.empty()) {
        return true;
    }

    fs::path p(_storagePath);
    if (p.has_parent_path() && !p.parent_path().empty()) {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        chmod(p.parent_path().c_str(), 0700);
    }

    json devicesArr = json::array();
    for (const auto &kv : _sessions) {
        const auto &s = kv.second;
        if (s.isRevoked) continue;
        devicesArr.push_back({
            {"device_id", s.deviceId},
            {"device_name", s.deviceName},
            {"token", s.token},
            {"paired_at", static_cast<int64_t>(s.pairedAt)},
            {"last_seen_at", static_cast<int64_t>(s.lastSeenAt)},
            {"is_revoked", s.isRevoked}
        });
    }

    json root;
    root["devices"] = devicesArr;

    std::string tmpPath = _storagePath + ".tmp";
    {
        std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            return false;
        }
        out << root.dump(2);
        out.flush();
        if (!out.good()) {
            return false;
        }
    }

    if (chmod(tmpPath.c_str(), 0600) != 0) {
        std::error_code rmEc;
        fs::remove(tmpPath, rmEc);
        return false;
    }

    std::error_code ec;
    fs::rename(tmpPath, _storagePath, ec);
    if (ec) {
        std::error_code rmEc;
        fs::remove(tmpPath, rmEc);
        return false;
    }

    chmod(_storagePath.c_str(), 0600);
    return true;
}

void MobileGateway::shutdown() {
    std::lock_guard<std::mutex> lock(_mutex);
    // Resolve all pending promises to prevent hanging threads
    for (auto &kv : _pendingApprovals) {
        if (kv.second && kv.second->promise) {
            try {
                kv.second->promise->set_value(ApprovalVerdict::DENIED);
            } catch (...) {}
        }
    }
    _pendingApprovals.clear();
    _resolvedApprovals.clear();
    _pairingSecrets.clear();
    _sessions.clear();
}

std::string MobileGateway::generateRandomHex(size_t byteCount) {
    static thread_local std::random_device rd;
    static thread_local std::mt19937_64 gen(rd());
    static thread_local std::uniform_int_distribution<uint8_t> dis(0, 255);

    std::ostringstream oss;
    for (size_t i = 0; i < byteCount; ++i) {
        oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(dis(gen));
    }
    return oss.str();
}

std::string MobileGateway::createPairingSecret(int validitySeconds) {
    std::lock_guard<std::mutex> lock(_mutex);
    time_t now = time(nullptr);

    // Purge expired secrets
    for (auto it = _pairingSecrets.begin(); it != _pairingSecrets.end();) {
        if (now > it->second) {
            it = _pairingSecrets.erase(it);
        } else {
            ++it;
        }
    }

    // If the last generated secret is still valid with at least 30s remaining, reuse it
    if (!_lastGeneratedSecret.empty() && now + 30 <= _lastGeneratedExpires) {
        auto it = _pairingSecrets.find(_lastGeneratedSecret);
        if (it != _pairingSecrets.end() && now <= it->second) {
            return _lastGeneratedSecret;
        }
    }

    int duration = (validitySeconds > 0 ? validitySeconds : 300);
    std::string secret = generateRandomHex(16); // 128-bit secret
    time_t expires = now + duration;
    _pairingSecrets[secret] = expires;
    _lastGeneratedSecret = secret;
    _lastGeneratedExpires = expires;

    return secret;
}

bool MobileGateway::pairDevice(const std::string &pairingSecret,
                               const std::string &deviceId,
                               const std::string &deviceName,
                               std::string &outToken) {
    std::lock_guard<std::mutex> lock(_mutex);
    time_t now = time(nullptr);

    // Purge expired secrets
    for (auto it = _pairingSecrets.begin(); it != _pairingSecrets.end();) {
        if (now > it->second) {
            it = _pairingSecrets.erase(it);
        } else {
            ++it;
        }
    }

    if (pairingSecret.empty()) {
        return false;
    }

    auto secIt = _pairingSecrets.find(pairingSecret);
    if (secIt == _pairingSecrets.end() || now > secIt->second) {
        return false;
    }

    outToken = generateRandomHex(32); // 256-bit session token

    std::string finalDeviceId = deviceId.empty() ? generateRandomHex(8) : deviceId;
    for (auto it = _sessions.begin(); it != _sessions.end();) {
        if (it->second.deviceId == finalDeviceId) {
            it = _sessions.erase(it);
        } else {
            ++it;
        }
    }

    MobileSession session;
    session.deviceId = finalDeviceId;
    session.deviceName = deviceName.empty() ? "Android Client" : deviceName;
    session.token = outToken;
    session.pairedAt = now;
    session.lastSeenAt = now;
    session.isRevoked = false;

    _sessions[outToken] = session;

    // Single-use pairing secret consumption
    _pairingSecrets.erase(secIt);
    if (_lastGeneratedSecret == pairingSecret) {
        _lastGeneratedSecret.clear();
        _lastGeneratedExpires = 0;
    }

    saveSessionsLocked();
    return true;
}

bool MobileGateway::authenticate(const std::string &token, std::string &outDeviceId) {
    if (token.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _sessions.find(token);
    if (it == _sessions.end() || it->second.isRevoked) {
        return false;
    }

    it->second.lastSeenAt = time(nullptr);
    outDeviceId = it->second.deviceId;
    return true;
}

void MobileGateway::revokeDevice(const std::string &deviceId) {
    if (deviceId.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    bool changed = false;
    for (auto &kv : _sessions) {
        if (kv.second.deviceId == deviceId && !kv.second.isRevoked) {
            kv.second.isRevoked = true;
            changed = true;
        }
    }
    if (changed) {
        saveSessionsLocked();
    }
}

std::vector<MobileSession> MobileGateway::listDevices() {
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<MobileSession> result;
    for (const auto &kv : _sessions) {
        if (!kv.second.isRevoked) {
            result.push_back(kv.second);
        }
    }
    return result;
}

std::string MobileGateway::submitApprovalRequest(const std::string &agentType,
                                                 const std::string &toolName,
                                                 const std::string &workspace,
                                                 const json &toolArgs,
                                                 const std::string &reason,
                                                 int timeoutSeconds) {
    auto req = std::make_shared<ApprovalRequest>();
    req->approvalId = generateRandomHex(16);
    req->agentType = agentType;
    req->toolName = toolName;
    req->workspace = workspace;
    req->toolArgs = toolArgs;
    req->reason = reason;
    req->requestedAt = time(nullptr);
    req->timeoutSeconds = timeoutSeconds > 0 ? timeoutSeconds : 120;
    req->verdict = ApprovalVerdict::PENDING;
    req->promise = std::make_shared<std::promise<ApprovalVerdict>>();

    {
        std::lock_guard<std::mutex> lock(_mutex);
        _pendingApprovals[req->approvalId] = req;
    }

    // Telemetry: record approval wait event
    AgentLifecycleEvent ev;
    ev.timestamp = req->requestedAt;
    ev.sessionId = "approval-" + req->approvalId;
    ev.agentType = agentType;
    ev.eventType = "APPROVAL_WAIT";
    ev.toolName = toolName;
    ev.status = "PENDING";
    json details;
    details["workspace"] = workspace;
    details["reason"] = reason;
    details["approval_id"] = req->approvalId;
    ev.detailsJson = details;
    AgentTelemetryDb::getInstance().insertEvent(ev);

    // WebSocket push broadcast to all connected mobile clients
    json wsPayload;
    wsPayload["approval_id"] = req->approvalId;
    wsPayload["agent_type"] = agentType;
    wsPayload["tool_name"] = toolName;
    wsPayload["workspace"] = workspace;
    wsPayload["tool_args"] = toolArgs;
    wsPayload["reason"] = reason;
    wsPayload["timeout_seconds"] = req->timeoutSeconds;
    wsPayload["remaining_seconds"] = req->timeoutSeconds;
    wsPayload["requested_at"] = req->requestedAt;
    broadcastWsMessage("approval_request", wsPayload);

    return req->approvalId;
}

ApprovalVerdict MobileGateway::waitForApproval(const std::string &approvalId, int timeoutSeconds) {
    std::shared_ptr<ApprovalRequest> req;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _pendingApprovals.find(approvalId);
        if (it == _pendingApprovals.end()) {
            auto resIt = _resolvedApprovals.find(approvalId);
            if (resIt != _resolvedApprovals.end()) {
                return resIt->second;
            }
            return ApprovalVerdict::TIMED_OUT;
        }
        req = it->second;
    }

    if (!req || !req->promise) {
        return ApprovalVerdict::TIMED_OUT;
    }

    auto future = req->promise->get_future();
    int waitSec = timeoutSeconds > 0 ? timeoutSeconds : req->timeoutSeconds;

    std::future_status status = future.wait_for(std::chrono::seconds(waitSec));
    ApprovalVerdict verdict = ApprovalVerdict::TIMED_OUT;

    if (status == std::future_status::ready) {
        verdict = future.get();
    } else {
        verdict = ApprovalVerdict::TIMED_OUT;
    }

    // Cleanup and finalize
    time_t now = time(nullptr);
    double waitLatencyMs = (now - req->requestedAt) * 1000.0;

    {
        std::lock_guard<std::mutex> lock(_mutex);
        _resolvedApprovals[approvalId] = verdict;
        _pendingApprovals.erase(approvalId);
    }

    // Telemetry: record approval verdict & latency
    AgentLifecycleEvent ev;
    ev.timestamp = now;
    ev.sessionId = "approval-" + approvalId;
    ev.agentType = req->agentType;
    ev.eventType = "APPROVAL_VERDICT";
    ev.toolName = req->toolName;
    ev.durationMs = waitLatencyMs;
    ev.status = (verdict == ApprovalVerdict::APPROVED) ? "APPROVED" :
                (verdict == ApprovalVerdict::DENIED) ? "DENIED" : "TIMED_OUT";
    json details;
    details["approval_id"] = approvalId;
    details["verdict"] = ev.status;
    details["latency_ms"] = waitLatencyMs;
    ev.detailsJson = details;
    AgentTelemetryDb::getInstance().insertEvent(ev);

    return verdict;
}

bool MobileGateway::resolveApproval(const std::string &approvalId, ApprovalVerdict verdict) {
    std::shared_ptr<ApprovalRequest> req;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _pendingApprovals.find(approvalId);
        if (it == _pendingApprovals.end()) {
            return false;
        }
        req = it->second;
        if (!req || req->verdict != ApprovalVerdict::PENDING) {
            return false;
        }
        req->verdict = verdict;
        _resolvedApprovals[approvalId] = verdict;
    }

    if (!req->promise) {
        return false;
    }

    try {
        req->promise->set_value(verdict);
    } catch (...) {
        return false;
    }

    // Broadcast verdict to other connected clients
    json wsPayload;
    wsPayload["approval_id"] = approvalId;
    wsPayload["verdict"] = (verdict == ApprovalVerdict::APPROVED) ? "APPROVED" : "DENIED";
    broadcastWsMessage("approval_resolved", wsPayload);

    return true;
}

std::vector<json> MobileGateway::listPendingApprovals() {
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<json> list;
    time_t now = time(nullptr);

    for (const auto &kv : _pendingApprovals) {
        const auto &req = kv.second;
        if (!req || req->verdict != ApprovalVerdict::PENDING) continue;

        int remaining = req->timeoutSeconds - static_cast<int>(now - req->requestedAt);
        if (remaining <= 0) continue;

        json item;
        item["approval_id"] = req->approvalId;
        item["agent_type"] = req->agentType;
        item["tool_name"] = req->toolName;
        item["workspace"] = req->workspace;
        item["tool_args"] = req->toolArgs;
        item["reason"] = req->reason;
        item["timeout_seconds"] = req->timeoutSeconds;
        item["remaining_seconds"] = remaining;
        item["requested_at"] = req->requestedAt;
        list.push_back(item);
    }

    return list;
}

json MobileGateway::getApprovalStatus(const std::string &approvalId) {
    std::lock_guard<std::mutex> lock(_mutex);
    time_t now = time(nullptr);

    auto it = _pendingApprovals.find(approvalId);
    if (it != _pendingApprovals.end() && it->second) {
        const auto &req = it->second;
        int remaining = req->timeoutSeconds - static_cast<int>(now - req->requestedAt);
        if (req->verdict == ApprovalVerdict::APPROVED) {
            return {
                {"id", approvalId},
                {"request_id", approvalId},
                {"approval_id", approvalId},
                {"status", "approved"},
                {"verdict", "APPROVED"},
                {"remaining_seconds", 0}
            };
        }
        if (req->verdict == ApprovalVerdict::DENIED) {
            return {
                {"id", approvalId},
                {"request_id", approvalId},
                {"approval_id", approvalId},
                {"status", "denied"},
                {"verdict", "DENIED"},
                {"remaining_seconds", 0}
            };
        }
        if (remaining <= 0) {
            return {
                {"id", approvalId},
                {"request_id", approvalId},
                {"approval_id", approvalId},
                {"status", "expired"},
                {"verdict", "TIMED_OUT"},
                {"remaining_seconds", 0}
            };
        }
        return {
            {"id", approvalId},
            {"request_id", approvalId},
            {"approval_id", approvalId},
            {"status", "pending"},
            {"verdict", "PENDING"},
            {"remaining_seconds", remaining}
        };
    }

    auto resIt = _resolvedApprovals.find(approvalId);
    if (resIt != _resolvedApprovals.end()) {
        std::string statusStr = (resIt->second == ApprovalVerdict::APPROVED) ? "approved" :
                                (resIt->second == ApprovalVerdict::DENIED) ? "denied" : "expired";
        std::string verdictStr = (resIt->second == ApprovalVerdict::APPROVED) ? "APPROVED" :
                                 (resIt->second == ApprovalVerdict::DENIED) ? "DENIED" : "TIMED_OUT";
        return {
            {"id", approvalId},
            {"request_id", approvalId},
            {"approval_id", approvalId},
            {"status", statusStr},
            {"verdict", verdictStr},
            {"remaining_seconds", 0}
        };
    }

    return {
        {"id", approvalId},
        {"request_id", approvalId},
        {"approval_id", approvalId},
        {"status", "not_found"},
        {"verdict", "TIMED_OUT"},
        {"remaining_seconds", 0}
    };
}

void MobileGateway::setBroadcastCallback(BroadcastCallback cb) {
    std::lock_guard<std::mutex> lock(_mutex);
    _broadcastCb = cb;
}

void MobileGateway::broadcastWsMessage(const std::string &type, const json &payload) {
    BroadcastCallback cb = nullptr;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        cb = _broadcastCb;
    }
    if (cb) {
        cb(type, payload);
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
