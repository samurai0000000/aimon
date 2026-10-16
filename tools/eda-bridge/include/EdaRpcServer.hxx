/*
 * EdaRpcServer.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_EDA_BRIDGE_RPC_SERVER_HXX
#define AIMON_EDA_BRIDGE_RPC_SERVER_HXX

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>
#include "ToolDiscovery.hxx"

namespace aimon {

struct EdaRpcServerOptions {
    std::string listenHost = "0.0.0.0";
    int port = 3892;
    int pluginPort = 3893;
    std::string authSecret;
    std::string allowedRoot;
    std::string kicadBinaryPath;
    std::string symbolLibDir;
    std::string footprintLibDir;
    std::string freecadBinaryPath;
    std::string easyedaBinaryPath;
    bool enableEasyEda = true;
    bool enableKiCad = true;
    bool enableFreeCad = true;
    bool disallowExports = false;
    int defaultTimeoutMs = 60000;
    size_t maxOutputBytes = 4 * 1024 * 1024;
};

struct PluginChunkState {
    int total = 0;
    int receivedCount = 0;
    std::vector<std::string> parts;
    std::vector<bool> received;
};

struct PluginSession {
    int sessionId = -1;
    int socketFd = -1;
    bool isWebSocket = false;
    std::string app; // "easyeda" | "kicad" | "freecad"
    std::string version;
    std::string activeDocument;
    std::atomic<bool> active{true};
    std::mutex writeMutex;
    std::unique_ptr<std::thread> readThread;
    std::map<std::string, PluginChunkState> chunkBuffers;
};

struct PluginPendingCall {
    std::string reqId;
    std::mutex mutex;
    std::condition_variable cv;
    bool completed = false;
    nlohmann::json result;
    std::string errorMessage;
};

class EdaRpcServer {
public:
    explicit EdaRpcServer(const EdaRpcServerOptions& options = EdaRpcServerOptions());
    ~EdaRpcServer();

    bool start();
    void stop();
    bool isRunning() const { return _running.load(); }

    int boundPort() const { return _boundPort.load(); }
    int boundPluginPort() const { return _boundPluginPort.load(); }

    bool connectToGateway(const std::string& host, int port);
    void disconnectGateway();
    bool isGatewayConnected() const { return _gatewayActive.load(); }

    void setBridgeEnabled(const std::string& app, bool enabled);
    bool isBridgeEnabled(const std::string& app) const;
    void setDisallowExports(bool disallow);
    bool isDisallowExports() const;
    void setAllowedRoot(const std::string& root);
    std::string allowedRoot() const;

    bool isPluginConnected(const std::string& app) const;

    nlohmann::json buildStatusPayload() const;
    nlohmann::json dispatchToolCall(const std::string& toolName,
                                    const nlohmann::json& arguments);

    static bool constantTimeEquals(const std::string& a, const std::string& b);
    bool validateSandboxPath(const std::string& pathStr, std::string& outError) const;
    bool validateJsonSandboxPaths(const nlohmann::json& payload, std::string& outError) const;

    static std::string computeWebSocketAcceptKey(const std::string& clientKey);
    static std::vector<uint8_t> encodeWebSocketFrame(const std::string& payload,
                                                     uint8_t opcode = 0x1,
                                                     bool mask = false);
    static bool decodeWebSocketFrame(const uint8_t* data,
                                     size_t len,
                                     size_t& outConsumed,
                                     uint8_t& outOpcode,
                                     std::string& outPayload,
                                     std::string& outError,
                                     bool* outFin = nullptr);

    static std::vector<nlohmann::json> remoteToolDescriptors();
    std::vector<nlohmann::json> enabledRemoteToolDescriptors() const;

private:
    void rpcAcceptLoop();
    void handleRpcClient(int clientFd);

    void pluginAcceptLoop();
    void handlePluginSession(std::shared_ptr<PluginSession> session);
    void processPluginMessage(std::shared_ptr<PluginSession> session,
                              const nlohmann::json& msg);
    bool sendPluginJson(std::shared_ptr<PluginSession> session,
                        const nlohmann::json& msg);
    bool callPlugin(const std::string& app,
                    const std::string& toolName,
                    const nlohmann::json& arguments,
                    nlohmann::json& outResult,
                    std::string& outError,
                    int timeoutMs);

    void gatewayReadLoop(int gwFd);

    nlohmann::json handleKiCadTool(const std::string& toolName, const nlohmann::json& args);
    nlohmann::json handleFreeCadTool(const std::string& toolName, const nlohmann::json& args);
    nlohmann::json handleEasyEdaTool(const std::string& toolName, const nlohmann::json& args);

    EdaRpcServerOptions _options;
    mutable std::mutex _allowedRootMutex;
    DiscoveredToolInfo _kicadInfo;
    DiscoveredToolInfo _freecadInfo;
    DiscoveredToolInfo _easyedaInfo;

    std::atomic<bool> _enableEasyEda{true};
    std::atomic<bool> _enableKiCad{true};
    std::atomic<bool> _enableFreeCad{true};
    std::atomic<bool> _disallowExports{false};

    std::atomic<bool> _running{false};
    int _rpcServerFd = -1;
    int _pluginServerFd = -1;
    std::atomic<int> _boundPort{0};
    std::atomic<int> _boundPluginPort{0};

    std::unique_ptr<std::thread> _rpcAcceptThread;
    std::unique_ptr<std::thread> _pluginAcceptThread;

    mutable std::mutex _pluginsMutex;
    std::map<int, std::shared_ptr<PluginSession>> _pluginSessions;
    std::atomic<int> _nextSessionId{1};

    std::mutex _pendingPluginMutex;
    std::map<std::string, std::shared_ptr<PluginPendingCall>> _pendingPluginCalls;
    std::atomic<uint64_t> _nextPluginReqId{1};

    int _gatewayFd = -1;
    std::atomic<bool> _gatewayActive{false};
    std::unique_ptr<std::thread> _gatewayThread;
    std::mutex _gatewayWriteMutex;

    mutable std::mutex _docStateMutex;
    std::map<std::string, nlohmann::json> _freecadDocs;
};

} // namespace aimon

#endif // AIMON_EDA_BRIDGE_RPC_SERVER_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
