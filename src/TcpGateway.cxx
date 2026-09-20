/*
 * TcpGateway.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "TcpGateway.hxx"
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>
#include <iostream>
#include <sstream>
#include <chrono>
#include <algorithm>
#include <map>

namespace aimon {

TcpGateway::TcpGateway(DynamicToolRegistry& registry,
                       ToolsChangedCallback onToolsChanged)
    : _registry(registry),
      _onToolsChanged(onToolsChanged) {
}

TcpGateway::~TcpGateway() {
    stop();
}

bool TcpGateway::start(const std::string& host, int port) {
    if (_running.load()) {
        return true;
    }

    _host = host;
    _port = port;

    _serverFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (_serverFd < 0) {
        std::cerr << "[TcpGateway] Failed to create socket: "
                  << ::strerror(errno) << std::endl;
        return false;
    }

    int opt = 1;
    if (::setsockopt(_serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "[TcpGateway] Warning: setsockopt SO_REUSEADDR failed: "
                  << ::strerror(errno) << std::endl;
    }

    struct sockaddr_in servAddr;
    ::memset(&servAddr, 0, sizeof(servAddr));
    servAddr.sin_family = AF_INET;
    servAddr.sin_port = htons(static_cast<uint16_t>(_port));

    if (_host.empty() || _host == "0.0.0.0") {
        servAddr.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (::inet_pton(AF_INET, _host.c_str(), &servAddr.sin_addr) <= 0) {
            std::cerr << "[TcpGateway] Invalid address: " << _host << std::endl;
            ::close(_serverFd);
            _serverFd = -1;
            return false;
        }
    }

    if (::bind(_serverFd, (struct sockaddr*)&servAddr, sizeof(servAddr)) < 0) {
        std::cerr << "[TcpGateway] Failed to bind to " << _host << ":" << _port
                  << ": " << ::strerror(errno) << std::endl;
        ::close(_serverFd);
        _serverFd = -1;
        return false;
    }

    if (::listen(_serverFd, 10) < 0) {
        std::cerr << "[TcpGateway] Failed to listen: "
                  << ::strerror(errno) << std::endl;
        ::close(_serverFd);
        _serverFd = -1;
        return false;
    }

    _running.store(true);
    _listenerThread = std::make_unique<std::thread>(&TcpGateway::listenerLoop, this);

    std::cout << "[TcpGateway] Listening on " << _host << ":" << _port
              << " for satellite tool registrations" << std::endl;
    return true;
}

void TcpGateway::stop() {
    if (!_running.exchange(false)) {
        return;
    }

    if (_serverFd >= 0) {
        ::shutdown(_serverFd, SHUT_RDWR);
        ::close(_serverFd);
        _serverFd = -1;
    }

    if (_listenerThread && _listenerThread->joinable()) {
        _listenerThread->join();
        _listenerThread.reset();
    }

    // Disconnect all clients
    {
        std::lock_guard<std::mutex> lock(_clientsMutex);
        for (auto& pair : _clients) {
            pair.second->active.store(false);
            if (pair.second->socketFd >= 0) {
                ::shutdown(pair.second->socketFd, SHUT_RDWR);
                ::close(pair.second->socketFd);
                pair.second->socketFd = -1;
            }
        }
        _clients.clear();
    }

    // Cancel all pending calls
    {
        std::lock_guard<std::mutex> lock(_pendingMutex);
        for (auto& pair : _pendingCalls) {
            std::lock_guard<std::mutex> callLock(pair.second->mutex);
            if (!pair.second->completed) {
                pair.second->errorMessage = "Gateway stopped";
                pair.second->completed = true;
                pair.second->cv.notify_all();
            }
        }
        _pendingCalls.clear();
    }

    _registry.clear();
}

void TcpGateway::setToolsChangedCallback(ToolsChangedCallback cb) {
    _onToolsChanged = cb;
}

void TcpGateway::listenerLoop() {
    while (_running.load()) {
        struct sockaddr_in clientAddr;
        socklen_t clientLen = sizeof(clientAddr);
        int clientFd = ::accept(_serverFd, (struct sockaddr*)&clientAddr, &clientLen);
        if (clientFd < 0) {
            if (!_running.load()) break;
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            std::cerr << "[TcpGateway] accept error: " << ::strerror(errno) << std::endl;
            break;
        }

        // Enable TCP_NODELAY
        int flag = 1;
        ::setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        char clientIp[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &clientAddr.sin_addr, clientIp, sizeof(clientIp));
        int clientPort = ntohs(clientAddr.sin_port);
        std::string remoteAddr = std::string(clientIp) + ":" + std::to_string(clientPort);

        auto client = std::make_shared<ClientConnection>();
        client->clientId = _nextClientId.fetch_add(1);
        client->socketFd = clientFd;
        client->remoteAddress = remoteAddr;
        client->active.store(true);

        std::cout << "[TcpGateway] Client connected: " << remoteAddr
                  << " (assigned client ID: " << client->clientId << ")" << std::endl;

        {
            std::lock_guard<std::mutex> lock(_clientsMutex);
            _clients[client->clientId] = client;
        }

        client->readThread = std::make_unique<std::thread>(&TcpGateway::clientReadLoop, this, client);
    }
}

void TcpGateway::clientReadLoop(std::shared_ptr<ClientConnection> client) {
    std::string buffer;
    char chunk[4096];

    while (_running.load() && client->active.load()) {
        ssize_t bytesRead = ::read(client->socketFd, chunk, sizeof(chunk));
        if (bytesRead <= 0) {
            // EOF or error
            break;
        }

        buffer.append(chunk, bytesRead);

        size_t newlinePos;
        while ((newlinePos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, newlinePos);
            buffer.erase(0, newlinePos + 1);

            // Strip trailing \r if present
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            if (line.empty()) continue;

            try {
                nlohmann::json j = nlohmann::json::parse(line, nullptr, false);
                if (j.is_discarded()) {
                    std::cerr << "[TcpGateway] Warning: malformed JSON from client "
                              << client->clientId << ": " << line << std::endl;
                    continue;
                }
                handleIncomingJson(client, j);
            } catch (const std::exception& e) {
                std::cerr << "[TcpGateway] Exception parsing line from client "
                          << client->clientId << ": " << e.what() << std::endl;
            }
        }
    }

    // Client disconnected
    client->active.store(false);
    if (client->socketFd >= 0) {
        ::close(client->socketFd);
        client->socketFd = -1;
    }

    std::cout << "[TcpGateway] Client disconnected: " << client->remoteAddress
              << " (" << client->subsystem << ", ID: " << client->clientId << ")" << std::endl;

    std::string peerIp = client->remoteAddress;
    size_t colon = peerIp.rfind(':');
    if (colon != std::string::npos) {
        peerIp = peerIp.substr(0, colon);
    }
    std::string retainedKey = client->subsystem + "@" + peerIp + ":" + std::to_string(client->webPort);

    {
        std::lock_guard<std::mutex> lock(_retainedMutex);
        auto it = _retainedMonitors.find(retainedKey);
        if (it != _retainedMonitors.end()) {
            it->second.connected = false;
            it->second.reachable = false;
            it->second.lastSeenEpoch = std::time(nullptr);
        }
    }

    std::vector<std::string> removedTools;
    _registry.unregisterClient(client->clientId, removedTools);

    {
        std::lock_guard<std::mutex> lock(_clientsMutex);
        _clients.erase(client->clientId);
    }

    // Notify in-flight calls waiting on this client
    {
        std::lock_guard<std::mutex> lock(_pendingMutex);
        for (auto& pair : _pendingCalls) {
            std::lock_guard<std::mutex> callLock(pair.second->mutex);
            if (!pair.second->completed) {
                pair.second->errorMessage = "Subsystem disconnected during tool execution";
                pair.second->completed = true;
                pair.second->cv.notify_all();
            }
        }
    }

    if (!removedTools.empty()) {
        std::cout << "[TcpGateway] Unregistered " << removedTools.size()
                  << " tools from " << client->subsystem << std::endl;
    }

    if (_onToolsChanged) {
        _onToolsChanged();
    }

    // Let thread detach itself
    if (client->readThread && client->readThread->joinable()) {
        client->readThread->detach();
    }
}

bool TcpGateway::sendLine(int socketFd, const std::string& line) {
    if (socketFd < 0) return false;
    std::string msg = line;
    if (msg.empty() || msg.back() != '\n') {
        msg.push_back('\n');
    }

    size_t totalSent = 0;
    while (totalSent < msg.size()) {
        ssize_t sent = ::write(socketFd, msg.data() + totalSent, msg.size() - totalSent);
        if (sent <= 0) {
            return false;
        }
        totalSent += sent;
    }
    return true;
}

void TcpGateway::handleIncomingJson(std::shared_ptr<ClientConnection> client,
                                   const nlohmann::json& msg) {
    if (msg.contains("method") && msg["method"].is_string()) {
        std::string method = msg["method"].get<std::string>();
        nlohmann::json id = msg.value("id", nlohmann::json(1));

        if (method == "gateway/register" || method == "gateway/updateTools") {
            nlohmann::json params = msg.value("params", nlohmann::json::object());
            std::string subsystem = params.value("subsystem", "unknown");
            client->subsystem = subsystem;

            if (params.contains("display_name") && params["display_name"].is_string()) {
                client->displayName = params["display_name"].get<std::string>();
            } else if (params.contains("title") && params["title"].is_string()) {
                client->displayName = params["title"].get<std::string>();
            } else if (params.contains("name") && params["name"].is_string()) {
                client->displayName = params["name"].get<std::string>();
            } else {
                client->displayName = subsystem;
            }

            if (params.contains("short_name") && params["short_name"].is_string()) {
                client->shortName = params["short_name"].get<std::string>();
            } else if (params.contains("shortName") && params["shortName"].is_string()) {
                client->shortName = params["shortName"].get<std::string>();
            } else {
                client->shortName = client->displayName;
            }

            if (params.contains("priority") && params["priority"].is_number()) {
                client->priority = params["priority"].get<int>();
            } else {
                if (subsystem == "aimon") client->priority = 0;
                else if (subsystem == "netmon") client->priority = 10;
                else if (subsystem == "meshmon") client->priority = 20;
                else if (subsystem == "embdevenv") client->priority = 30;
                else client->priority = 100;
            }

            if (params.contains("web_port") && params["web_port"].is_number()) {
                client->webPort = params["web_port"].get<int>();
            } else {
                // Fallback default ports for known subsystems if not explicitly sent
                if (subsystem == "netmon") client->webPort = 3884;
                else if (subsystem == "meshmon") client->webPort = 16880;
                else if (subsystem == "embdevenv") client->webPort = 3886;
            }

            if (params.contains("web_path") && params["web_path"].is_string()) {
                client->webPath = params["web_path"].get<std::string>();
            } else {
                client->webPath = "/";
            }

            std::string peerIp = client->remoteAddress;
            size_t colon = peerIp.rfind(':');
            if (colon != std::string::npos) {
                peerIp = peerIp.substr(0, colon);
            }
            std::string key = client->subsystem + "@" + peerIp + ":" + std::to_string(client->webPort);
            bool reachable = (client->webPort > 0) ? checkTcpPortReachable(peerIp, client->webPort, 800) : false;
            client->webReachable.store(reachable);
            client->lastSeenEpoch.store(std::time(nullptr));

            {
                std::lock_guard<std::mutex> lock(_retainedMutex);
                DiscoveredMonitor dm;
                dm.id = client->subsystem;
                dm.name = client->displayName;
                dm.shortName = client->shortName;
                dm.subsystem = client->subsystem;
                dm.host = peerIp;
                dm.port = client->webPort;
                dm.path = client->webPath;
                dm.connected = true;
                dm.reachable = reachable;
                dm.isSelf = false;
                dm.priority = client->priority;
                dm.lastSeenEpoch = std::time(nullptr);
                _retainedMonitors[key] = dm;
            }

            nlohmann::json toolsArray = params.value("tools", nlohmann::json::array());
            std::vector<std::string> addedNames;
            bool ok = _registry.registerTools(client->clientId, subsystem, toolsArray, addedNames);

            std::cout << "[TcpGateway] " << (method == "gateway/register" ? "Registered" : "Updated")
                      << " " << addedNames.size() << " tools from " << subsystem
                      << " (" << client->displayName << " [" << client->shortName << "], priority " << client->priority
                      << ", client " << client->clientId << ", web_port: " << client->webPort << ")" << std::endl;

            nlohmann::json resp = {
                {"jsonrpc", "2.0"},
                {"id", id},
                {"result", {
                    {"status", ok ? "registered" : "empty"},
                    {"registered_tools", addedNames.size()}
                }}
            };
            sendLine(client->socketFd, resp.dump());

            if (_onToolsChanged) {
                _onToolsChanged();
            }
        } else if (method == "ping") {
            nlohmann::json resp = {
                {"jsonrpc", "2.0"},
                {"id", id},
                {"result", nlohmann::json::object()}
            };
            sendLine(client->socketFd, resp.dump());
        }
        return;
    }

    // Check if this is a response to an in-flight tool call
    if (msg.contains("id")) {
        std::string reqId;
        if (msg["id"].is_string()) {
            reqId = msg["id"].get<std::string>();
        } else if (msg["id"].is_number()) {
            reqId = std::to_string(msg["id"].get<int64_t>());
        }

        std::shared_ptr<PendingCall> call;
        {
            std::lock_guard<std::mutex> lock(_pendingMutex);
            auto it = _pendingCalls.find(reqId);
            if (it != _pendingCalls.end()) {
                call = it->second;
            }
        }

        if (call) {
            std::lock_guard<std::mutex> lock(call->mutex);
            call->response = msg;
            call->completed = true;
            call->cv.notify_one();
        }
    }
}

bool TcpGateway::callTool(const std::string& toolName,
                         const nlohmann::json& arguments,
                         nlohmann::json& outResult,
                         std::string& outErrorMessage,
                         int timeoutMs) {
    int clientId = _registry.getToolClientId(toolName);
    if (clientId < 0) {
        outErrorMessage = "Tool '" + toolName + "' is not registered";
        return false;
    }

    std::shared_ptr<ClientConnection> client;
    {
        std::lock_guard<std::mutex> lock(_clientsMutex);
        auto it = _clients.find(clientId);
        if (it != _clients.end()) {
            client = it->second;
        }
    }

    if (!client || !client->active.load() || client->socketFd < 0) {
        outErrorMessage = "Subsystem host for tool '" + toolName + "' is offline";
        return false;
    }

    std::string reqId = "aimon-gw-" + std::to_string(_nextRequestId.fetch_add(1));
    auto pending = std::make_shared<PendingCall>();
    pending->reqId = reqId;

    {
        std::lock_guard<std::mutex> lock(_pendingMutex);
        _pendingCalls[reqId] = pending;
    }

    nlohmann::json reqJson = {
        {"jsonrpc", "2.0"},
        {"id", reqId},
        {"method", "tools/call"},
        {"params", {
            {"name", toolName},
            {"arguments", arguments.is_object() ? arguments : nlohmann::json::object()}
        }}
    };

    if (!sendLine(client->socketFd, reqJson.dump())) {
        std::lock_guard<std::mutex> lock(_pendingMutex);
        _pendingCalls.erase(reqId);
        outErrorMessage = "Failed to send request to subsystem '" + client->subsystem + "'";
        return false;
    }

    std::unique_lock<std::mutex> lock(pending->mutex);
    bool received = pending->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&]() {
        return pending->completed;
    });

    {
        std::lock_guard<std::mutex> pLock(_pendingMutex);
        _pendingCalls.erase(reqId);
    }

    if (!received) {
        outErrorMessage = "Timed out waiting for subsystem '" + client->subsystem + "'";
        return false;
    }

    if (!pending->errorMessage.empty()) {
        outErrorMessage = pending->errorMessage;
        return false;
    }

    if (pending->response.contains("error") && !pending->response["error"].is_null()) {
        outErrorMessage = pending->response["error"].dump();
        return false;
    }

    if (pending->response.contains("result")) {
        outResult = pending->response["result"];
        return true;
    }

    outErrorMessage = "Malformed response from subsystem '" + client->subsystem + "'";
    return false;
}

bool TcpGateway::checkTcpPortReachable(const std::string& host, int port, int timeoutMs) {
    if (host.empty() || port <= 0 || port > 65535) {
        return false;
    }

    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return false;
    }

    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        ::close(sock);
        return false;
    }

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        ::close(sock);
        return false;
    }

    int res = ::connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (res < 0) {
        if (errno == EINPROGRESS) {
            fd_set wset;
            FD_ZERO(&wset);
            FD_SET(sock, &wset);
            struct timeval tv;
            tv.tv_sec = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;
            int sel = ::select(sock + 1, nullptr, &wset, nullptr, &tv);
            if (sel > 0) {
                int err = 0;
                socklen_t len = sizeof(err);
                if (::getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) {
                    res = 0;
                } else {
                    res = -1;
                }
            } else {
                res = -1;
            }
        } else {
            res = -1;
        }
    }

    ::close(sock);
    return (res == 0);
}

std::vector<DiscoveredMonitor> TcpGateway::getDiscoveredMonitors(int selfWebPort) const {
    std::vector<DiscoveredMonitor> list;

    // Self: aimon
    DiscoveredMonitor selfMon;
    selfMon.id = "aimon";
    selfMon.name = "AI Quotas";
    selfMon.shortName = "AI Quotas";
    selfMon.subsystem = "aimon";
    selfMon.host = "127.0.0.1";
    selfMon.port = selfWebPort;
    selfMon.path = "/";
    selfMon.connected = true;
    selfMon.reachable = true;
    selfMon.isSelf = true;
    selfMon.priority = 0;
    selfMon.lastSeenEpoch = std::time(nullptr);
    list.push_back(selfMon);

    std::lock_guard<std::mutex> lock(_retainedMutex);

    // Count per subsystem for multi-host disambiguation
    std::map<std::string, int> countPerSubsystem;
    for (const auto& pair : _retainedMonitors) {
        countPerSubsystem[pair.second.subsystem]++;
    }

    for (auto pair : _retainedMonitors) {
        DiscoveredMonitor mon = pair.second;
        if (mon.port <= 0) continue;

        if (mon.connected) {
            mon.reachable = checkTcpPortReachable(mon.host, mon.port, 200);
        } else {
            mon.reachable = false;
        }

        auto sanitizeHost = [](const std::string& h) -> std::string {
            std::string s = h;
            for (char& c : s) {
                if (c == '.' || c == ':') c = '-';
            }
            return s;
        };

        mon.id = mon.subsystem + "-" + sanitizeHost(mon.host) + "-" + std::to_string(mon.port);

        bool hasMultiple = (countPerSubsystem[mon.subsystem] > 1);
        std::string baseName = !mon.name.empty() ? mon.name : mon.subsystem;
        std::string baseShort = !mon.shortName.empty() ? mon.shortName : baseName;

        if (hasMultiple) {
            mon.name = baseName + " (" + mon.host + ")";
            mon.shortName = baseShort + "@" + mon.host;
        } else {
            mon.name = baseName;
            mon.shortName = baseShort;
        }

        list.push_back(mon);
    }

    std::sort(list.begin(), list.end(), [](const DiscoveredMonitor& a, const DiscoveredMonitor& b) {
        if (a.priority != b.priority) return a.priority < b.priority;
        if (a.subsystem != b.subsystem) return a.subsystem < b.subsystem;
        if (a.host != b.host) return a.host < b.host;
        return a.port < b.port;
    });

    return list;
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
