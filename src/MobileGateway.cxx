/*
 * MobileGateway.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "MobileGateway.hxx"
#include "AgentTelemetryDb.hxx"
#include <random>
#include <sstream>
#include <iomanip>
#include <iostream>

using json = nlohmann::json;

namespace aimon {

MobileGateway &MobileGateway::getInstance() {
    static MobileGateway instance;
    return instance;
}

MobileGateway::MobileGateway()
    : _pairingSecret("")
    , _pairingExpires(0)
    , _broadcastCb(nullptr) {
}

MobileGateway::~MobileGateway() {
    shutdown();
}

bool MobileGateway::init(const std::string &storagePath) {
    // MobileGateway in-memory latch and session initialization
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
    _pairingSecret = generateRandomHex(16); // 128-bit secret
    _pairingExpires = time(nullptr) + (validitySeconds > 0 ? validitySeconds : 300);
    return _pairingSecret;
}

bool MobileGateway::pairDevice(const std::string &pairingSecret,
                               const std::string &deviceId,
                               const std::string &deviceName,
                               std::string &outToken) {
    std::lock_guard<std::mutex> lock(_mutex);
    time_t now = time(nullptr);

    if (_pairingSecret.empty() || now > _pairingExpires || pairingSecret != _pairingSecret) {
        return false;
    }

    outToken = generateRandomHex(32); // 256-bit session token

    MobileSession session;
    session.deviceId = deviceId.empty() ? generateRandomHex(8) : deviceId;
    session.deviceName = deviceName.empty() ? "Android Client" : deviceName;
    session.token = outToken;
    session.pairedAt = now;
    session.lastSeenAt = now;
    session.isRevoked = false;

    _sessions[outToken] = session;

    // Single-use pairing secret consumption
    _pairingSecret.clear();
    _pairingExpires = 0;

    return true;
}

bool MobileGateway::authenticate(const std::string &token, std::string &outDeviceId) {
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
    std::lock_guard<std::mutex> lock(_mutex);
    for (auto &kv : _sessions) {
        if (kv.second.deviceId == deviceId) {
            kv.second.isRevoked = true;
        }
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
    }

    if (!req || !req->promise) {
        return false;
    }

    req->verdict = verdict;
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
        if (!req) continue;

        int remaining = req->timeoutSeconds - static_cast<int>(now - req->requestedAt);
        if (remaining <= 0) continue;

        json item;
        item["approval_id"] = req->approvalId;
        item["agent_type"] = req->agentType;
        item["tool_name"] = req->toolName;
        item["workspace"] = req->workspace;
        item["tool_args"] = req->toolArgs;
        item["reason"] = req->reason;
        item["remaining_seconds"] = remaining;
        item["requested_at"] = req->requestedAt;
        list.push_back(item);
    }

    return list;
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
