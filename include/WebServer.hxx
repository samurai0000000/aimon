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
class TcpGateway;
class InsightEngine;
class MobileGateway;
class EdaBridge;

struct SseSession {
    std::string id;
    std::string profile;
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
              McpServer* mcpServer = nullptr,
              TcpGateway* tcpGateway = nullptr);
    ~WebServer();

    bool start(bool async = false);
    void stop();
    void broadcastSseNotification(const std::string& jsonRpcNotification);
    void scanTranscriptsForNotifications(bool seedOnly = false);

    void setInsightEngine(InsightEngine* engine) {
        _insightEngine = engine;
    }

    void setMobileGateway(MobileGateway* gateway) {
        _mobileGateway = gateway;
    }

    void setEdaBridge(EdaBridge* edaBridge) {
        _edaBridge = edaBridge;
    }

    void setConfigManager(ConfigManager* configManager) {
        _configManager = configManager;
        if (_configManager) {
            std::lock_guard<std::mutex> lock(_gatewayConfigMutex);
            _runtimeGatewayConfig = _configManager->getConfigSnapshot().gateway;
        }
    }

    void setGatewayConfig(const GatewayConfig& cfg) {
        std::lock_guard<std::mutex> lock(_gatewayConfigMutex);
        _runtimeGatewayConfig = cfg;
        _hasExplicitGatewayConfig = true;
    }

    GatewayConfig getGatewayConfig() const {
        std::lock_guard<std::mutex> lock(_gatewayConfigMutex);
        if (_hasExplicitGatewayConfig) {
            return _runtimeGatewayConfig;
        }
        if (_configManager) {
            return _configManager->getConfigSnapshot().gateway;
        }
        return _runtimeGatewayConfig;
    }

    int getPort() const {
        return _config.port;
    }

private:
    void setupRoutes();

    std::string createUiSession();
    bool isValidUiSession(const httplib::Request& req) const;
    bool isValidMobileToken(const httplib::Request& req) const;
    bool isValidMobileOrUiRequest(const httplib::Request& req) const;
    static std::string extractMobileToken(const httplib::Request& req);
    static bool isLoopbackRequest(const httplib::Request& req);
    static std::string getMcpHintForPath(const std::string& path, const std::string& body);

    StateStore& _stateStore;
    HistoryStore& _historyStore;
    WebConfig _config;
    RefreshCallback _onRefresh;
    McpServer* _mcpServer = nullptr;
    TcpGateway* _tcpGateway = nullptr;
    InsightEngine* _insightEngine = nullptr;
    MobileGateway* _mobileGateway = nullptr;
    EdaBridge* _edaBridge = nullptr;
    ConfigManager* _configManager = nullptr;
    mutable std::mutex _gatewayConfigMutex;
    GatewayConfig _runtimeGatewayConfig;
    bool _hasExplicitGatewayConfig = false;

    bool _endpointsEnabled = false;
    mutable std::mutex _sessionMutex;
    std::map<std::string, time_t> _uiSessions;

    std::map<std::string, std::shared_ptr<SseSession>> _sseSessions;
    std::mutex _sessionsMutex;

    struct WatchedTranscriptState {
        int64_t lastMtimeTicks = 0;
        uintmax_t lastFileSize = 0;
        int lastNotifiedStepIndex = -1;
        int pendingStepIndex = -1;
        std::string pendingEvent;
        std::string pendingSnippet;
        std::chrono::steady_clock::time_point pendingSince{};
        std::string cachedTitle;
        std::string cachedWorkspace;
        bool seeded = false;
        bool isSubagent = false;
    };
    std::mutex _watchedTranscriptsMutex;
    std::map<std::string, WatchedTranscriptState> _watchedTranscripts;
    std::unique_ptr<std::thread> _notificationThread;
    std::mutex _notificationStopMutex;
    std::condition_variable _notificationStopCv;

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
