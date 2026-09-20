/*
 * TcpGateway.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_TCP_GATEWAY_HXX
#define AIMON_TCP_GATEWAY_HXX

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <functional>
#include <nlohmann/json.hpp>
#include "DynamicToolRegistry.hxx"
#include "Models.hxx"

namespace aimon {

struct ClientConnection {
    int clientId = -1;
    int socketFd = -1;
    std::string remoteAddress;
    std::string subsystem;
    std::string displayName;
    std::string shortName;
    int priority = 100;
    int webPort = 0;
    std::string webPath = "/";
    std::atomic<bool> active{true};
    std::atomic<bool> webReachable{false};
    std::atomic<int64_t> lastSeenEpoch{0};
    std::unique_ptr<std::thread> readThread;
};

struct PendingCall {
    std::string reqId;
    std::mutex mutex;
    std::condition_variable cv;
    bool completed = false;
    nlohmann::json response;
    std::string errorMessage;
};

class TcpGateway {
public:
    using ToolsChangedCallback = std::function<void()>;

    TcpGateway(DynamicToolRegistry& registry,
               ToolsChangedCallback onToolsChanged = nullptr);
    ~TcpGateway();

    bool start(const std::string& host, int port);
    void stop();
    bool isRunning() const { return _running.load(); }

    bool callTool(const std::string& toolName,
                  const nlohmann::json& arguments,
                  nlohmann::json& outResult,
                  std::string& outErrorMessage,
                  int timeoutMs = 45000);

    void setToolsChangedCallback(ToolsChangedCallback cb);

    std::vector<DiscoveredMonitor> getDiscoveredMonitors(int selfWebPort = 3883) const;
    static bool checkTcpPortReachable(const std::string& host, int port, int timeoutMs = 800);

private:
    void listenerLoop();
    void clientReadLoop(std::shared_ptr<ClientConnection> client);
    bool sendLine(int socketFd, const std::string& line);
    void handleIncomingJson(std::shared_ptr<ClientConnection> client,
                            const nlohmann::json& msg);

    DynamicToolRegistry& _registry;
    ToolsChangedCallback _onToolsChanged;

    std::string _host = "0.0.0.0";
    int _port = 3885;
    int _serverFd = -1;
    std::atomic<bool> _running{false};
    std::unique_ptr<std::thread> _listenerThread;

    std::mutex _clientsMutex;
    std::map<int, std::shared_ptr<ClientConnection>> _clients;
    std::atomic<int> _nextClientId{1};

    std::mutex _pendingMutex;
    std::map<std::string, std::shared_ptr<PendingCall>> _pendingCalls;
    std::atomic<uint64_t> _nextRequestId{1};

    mutable std::mutex _retainedMutex;
    std::map<std::string, DiscoveredMonitor> _retainedMonitors;
};

} // namespace aimon

#endif // AIMON_TCP_GATEWAY_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
