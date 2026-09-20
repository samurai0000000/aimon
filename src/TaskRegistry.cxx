/*
 * TaskRegistry.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "TaskRegistry.hxx"
#include <mutex>
#include <random>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace aimon {

TaskRegistry& TaskRegistry::getInstance() {
    static TaskRegistry instance;
    return instance;
}

TaskRegistry::TaskRegistry() {
}

void TaskRegistry::registerSession(const std::string& sessionId, const std::string& remoteIp,
                                   const std::string& defaultClientName) {
    if (sessionId.empty()) {
        return;
    }

    auto nowEpoch = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::unique_lock<std::shared_mutex> lock(_mutex);
    ClientSession session;
    session.sessionId = sessionId;
    session.remoteIp = remoteIp;
    session.clientName = defaultClientName;
    session.connectedTimeEpoch = nowEpoch;
    session.lastHeartbeatEpoch = nowEpoch;
    session.active = true;
    _sessions[sessionId] = session;
}

void TaskRegistry::updateSessionClientInfo(const std::string& sessionId, const std::string& clientName,
                                          const std::string& clientVersion) {
    if (sessionId.empty()) {
        return;
    }

    auto nowEpoch = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::unique_lock<std::shared_mutex> lock(_mutex);
    auto it = _sessions.find(sessionId);
    if (it != _sessions.end()) {
        if (!clientName.empty()) {
            it->second.clientName = clientName;
        }
        if (!clientVersion.empty()) {
            it->second.clientVersion = clientVersion;
        }
        it->second.lastHeartbeatEpoch = nowEpoch;
        it->second.active = true;
    }
}

void TaskRegistry::touchSession(const std::string& sessionId) {
    if (sessionId.empty()) {
        return;
    }

    auto nowEpoch = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::unique_lock<std::shared_mutex> lock(_mutex);
    auto it = _sessions.find(sessionId);
    if (it != _sessions.end()) {
        it->second.lastHeartbeatEpoch = nowEpoch;
        it->second.active = true;
    }
}

void TaskRegistry::removeSession(const std::string& sessionId) {
    if (sessionId.empty()) {
        return;
    }

    std::unique_lock<std::shared_mutex> lock(_mutex);
    _sessions.erase(sessionId);
}

std::vector<ClientSession> TaskRegistry::listSessions() const {
    std::shared_lock<std::shared_mutex> lock(_mutex);
    std::vector<ClientSession> result;
    result.reserve(_sessions.size());
    for (const auto& pair : _sessions) {
        result.push_back(pair.second);
    }
    return result;
}

bool TaskRegistry::getSessionClientName(const std::string& sessionId, std::string& outClientName) const {
    if (sessionId.empty()) {
        return false;
    }

    std::shared_lock<std::shared_mutex> lock(_mutex);
    auto it = _sessions.find(sessionId);
    if (it != _sessions.end() && !it->second.clientName.empty() && it->second.clientName != "MCP Client") {
        outClientName = it->second.clientName;
        return true;
    }
    return false;
}

void TaskRegistry::clear() {
    std::unique_lock<std::shared_mutex> lock(_mutex);
    _sessions.clear();
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
