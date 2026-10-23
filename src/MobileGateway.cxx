/*
 * MobileGateway.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "MobileGateway.hxx"
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
