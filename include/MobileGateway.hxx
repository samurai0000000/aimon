/*
 * MobileGateway.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_MOBILEGATEWAY_HXX
#define AIMON_MOBILEGATEWAY_HXX

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <future>
#include <chrono>
#include <nlohmann/json.hpp>

namespace aimon {

enum class ApprovalVerdict {
    PENDING,
    APPROVED,
    DENIED,
    TIMED_OUT
};

struct MobileSession {
    std::string deviceId;
    std::string deviceName;
    std::string token;
    time_t      pairedAt = 0;
    time_t      lastSeenAt = 0;
    bool        isRevoked = false;
};

struct ApprovalRequest {
    std::string approvalId;
    std::string agentType;
    std::string toolName;
    std::string workspace;
    nlohmann::json toolArgs;
    std::string reason;
    time_t requestedAt = 0;
    int timeoutSeconds = 120;
    ApprovalVerdict verdict = ApprovalVerdict::PENDING;
    std::shared_ptr<std::promise<ApprovalVerdict>> promise;
};

class MobileGateway {
public:
    static MobileGateway &getInstance();

    bool init(const std::string &storagePath = "");
    void shutdown();

    // Pairing & Authentication
    std::string createPairingSecret(int validitySeconds = 300);
    bool pairDevice(const std::string &pairingSecret,
                    const std::string &deviceId,
                    const std::string &deviceName,
                    std::string &outToken);
    bool authenticate(const std::string &token, std::string &outDeviceId);
    void revokeDevice(const std::string &deviceId);
    std::vector<MobileSession> listDevices();

    // Action Approvals
    std::string submitApprovalRequest(const std::string &agentType,
                                      const std::string &toolName,
                                      const std::string &workspace,
                                      const nlohmann::json &toolArgs,
                                      const std::string &reason = "",
                                      int timeoutSeconds = 120);
    ApprovalVerdict waitForApproval(const std::string &approvalId, int timeoutSeconds = 120);
    bool resolveApproval(const std::string &approvalId, ApprovalVerdict verdict);
    std::vector<nlohmann::json> listPendingApprovals();

    // Notification / WS Broadcast Dispatcher Callback
    using BroadcastCallback = std::function<void(const std::string &type, const nlohmann::json &payload)>;
    void setBroadcastCallback(BroadcastCallback cb);
    void broadcastWsMessage(const std::string &type, const nlohmann::json &payload);

private:
    MobileGateway();
    ~MobileGateway();
    MobileGateway(const MobileGateway &) = delete;
    MobileGateway &operator=(const MobileGateway &) = delete;

    std::string generateRandomHex(size_t byteCount);

    std::mutex _mutex;
    std::string _pairingSecret;
    time_t _pairingExpires = 0;
    std::map<std::string, MobileSession> _sessions;
    std::map<std::string, std::shared_ptr<ApprovalRequest>> _pendingApprovals;
    BroadcastCallback _broadcastCb;
};

} // namespace aimon

#endif /* AIMON_MOBILEGATEWAY_HXX */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
