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
#include <functional>
#include <chrono>
#include <nlohmann/json.hpp>

namespace aimon {

struct MobileSession {
    std::string deviceId;
    std::string deviceName;
    std::string token;
    time_t      pairedAt = 0;
    time_t      lastSeenAt = 0;
    bool        isRevoked = false;
};

class MobileGateway {
public:
    static MobileGateway &getInstance();

    MobileGateway();
    ~MobileGateway();

    bool init(const std::string &storagePath = "");
    void shutdown();

    bool loadSessions();
    bool saveSessions();

    // Pairing & Authentication
    std::string createPairingSecret(int validitySeconds = 300);
    bool pairDevice(const std::string &pairingSecret,
                    const std::string &deviceId,
                    const std::string &deviceName,
                    std::string &outToken);
    bool verifyPairingCode(const std::string &pairingSecret,
                           const std::string &deviceId,
                           const std::string &deviceName,
                           std::string &outToken) {
        return pairDevice(pairingSecret, deviceId, deviceName, outToken);
    }
    std::string verifyPairingCode(const std::string &pairingSecret,
                                  const std::string &deviceName = "Android Client",
                                  const std::string &deviceId = "") {
        std::string token;
        if (pairDevice(pairingSecret, deviceId, deviceName, token)) {
            return token;
        }
        return "";
    }
    bool authenticate(const std::string &token, std::string &outDeviceId);
    bool validateToken(const std::string &token) {
        std::string devId;
        return authenticate(token, devId);
    }
    bool validateToken(const std::string &token, std::string &outDeviceId) {
        return authenticate(token, outDeviceId);
    }
    void revokeDevice(const std::string &deviceId);
    std::vector<MobileSession> listDevices();
    std::vector<MobileSession> getPairedDevices() {
        return listDevices();
    }

    // Notification / WS Broadcast Dispatcher Callback
    using BroadcastCallback = std::function<void(const std::string &type, const nlohmann::json &payload)>;
    void setBroadcastCallback(BroadcastCallback cb);
    void broadcastWsMessage(const std::string &type, const nlohmann::json &payload);

private:
    MobileGateway(const MobileGateway &) = delete;
    MobileGateway &operator=(const MobileGateway &) = delete;

    bool loadSessionsLocked();
    bool saveSessionsLocked();
    std::string generateRandomHex(size_t byteCount);

    std::mutex _mutex;
    std::string _storagePath;
    std::string _lastGeneratedSecret;
    time_t _lastGeneratedExpires = 0;
    std::map<std::string, time_t> _pairingSecrets;
    std::map<std::string, MobileSession> _sessions;
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
