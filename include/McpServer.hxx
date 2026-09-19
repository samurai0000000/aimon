/*
 * McpServer.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_MCP_SERVER_HXX
#define AIMON_MCP_SERVER_HXX

#include <string>
#include <functional>
#include "StateStore.hxx"
#include <nlohmann/json.hpp>

namespace aimon {

class DynamicToolRegistry;
class TcpGateway;
class CollabOrchestrator;

class McpServer {
public:
    explicit McpServer(StateStore& stateStore,
                       DynamicToolRegistry* dynamicRegistry = nullptr,
                       TcpGateway* tcpGateway = nullptr);

    void run();
    nlohmann::json handleMessage(const nlohmann::json& request);

    void setDynamicRegistry(DynamicToolRegistry* reg) { _dynamicRegistry = reg; }
    void setTcpGateway(TcpGateway* gw) { _tcpGateway = gw; }
    void setNotificationBroadcaster(std::function<void(const std::string&)> broadcaster) {
        _notificationBroadcaster = broadcaster;
    }
    void setCollabOrchestrator(CollabOrchestrator* orch) { _collabOrch = orch; }
    void notifyToolsListChanged();

private:
    nlohmann::json handleInitialize(const nlohmann::json& id, const nlohmann::json& params);
    nlohmann::json handleToolsList(const nlohmann::json& id);
    nlohmann::json handleToolsCall(const nlohmann::json& id, const nlohmann::json& params);

    std::string formatAntigravityStatus(const AntigravityStatus& ag);
    std::string formatCursorStatus(const CursorStatus& cr);
    std::string formatCombinedStatus(const AggregateStatus& status);
    std::string formatAgentTasks(const std::vector<AgentTask>& tasks);

    StateStore& _stateStore;
    DynamicToolRegistry* _dynamicRegistry = nullptr;
    TcpGateway* _tcpGateway = nullptr;
    CollabOrchestrator* _collabOrch = nullptr;
    std::function<void(const std::string&)> _notificationBroadcaster;
};

} // namespace aimon

#endif // AIMON_MCP_SERVER_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
