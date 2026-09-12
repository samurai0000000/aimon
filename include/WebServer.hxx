/*
 * WebServer.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_WEB_SERVER_HXX
#define AIMON_WEB_SERVER_HXX

#include <string>
#include <memory>
#include <thread>
#include <functional>
#include <map>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include "StateStore.hxx"
#include "HistoryStore.hxx"
#include "ConfigManager.hxx"

namespace httplib {
class Server;
class Request;
class Response;
}

namespace aimon {

class McpServer;

struct SseSession {
    std::string id;
    std::queue<std::string> messageQueue;
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> closed{false};
};

class WebServer {
public:
    using RefreshCallback = std::function<void()>;

    WebServer(StateStore& stateStore, HistoryStore& historyStore,
              const WebConfig& config, RefreshCallback onRefresh = nullptr,
              McpServer* mcpServer = nullptr);
    ~WebServer();

    bool start(bool async = false);
    void stop();
    void broadcastSseNotification(const std::string& jsonRpcNotification);

private:
    void setupRoutes();

    StateStore& _stateStore;
    HistoryStore& _historyStore;
    WebConfig _config;
    RefreshCallback _onRefresh;
    McpServer* _mcpServer = nullptr;

    std::map<std::string, std::shared_ptr<SseSession>> _sseSessions;
    std::mutex _sessionsMutex;

    std::unique_ptr<httplib::Server> _server;
    std::unique_ptr<std::thread> _thread;
    bool _running = false;
};

} // namespace aimon

#endif // AIMON_WEB_SERVER_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
