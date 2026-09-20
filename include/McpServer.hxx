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

class McpServer {
public:
    explicit McpServer(StateStore& stateStore,
                       DynamicToolRegistry* dynamicRegistry = nullptr,
                       TcpGateway* tcpGateway = nullptr);

    void run();
    nlohmann::json handleMessage(const nlohmann::json& request, const std::string& profile = "");

    void setDynamicRegistry(DynamicToolRegistry* reg) { _dynamicRegistry = reg; }
    void setTcpGateway(TcpGateway* gw) { _tcpGateway = gw; }
    void setNotificationBroadcaster(std::function<void(const std::string&)> broadcaster) {
        _notificationBroadcaster = broadcaster;
    }
    void setDefaultProfile(const std::string& profile) { _defaultProfile = profile; }
    const std::string& getDefaultProfile() const { return _defaultProfile; }
    void notifyToolsListChanged();

    static bool isToolAllowedInProfile(const std::string& toolName, const std::string& profile);
    static std::string detectProfileFromWorkspace(const std::string& workspacePath);

private:
    nlohmann::json handleInitialize(const nlohmann::json& id, const nlohmann::json& params);
    nlohmann::json handleToolsList(const nlohmann::json& id, const std::string& profile);
    nlohmann::json handleToolsCall(const nlohmann::json& id, const nlohmann::json& params, const std::string& profile);

    std::string formatAntigravityStatus(const AntigravityStatus& ag);
    std::string formatCursorStatus(const CursorStatus& cr);
    std::string formatCombinedStatus(const AggregateStatus& status);

    StateStore& _stateStore;
    DynamicToolRegistry* _dynamicRegistry = nullptr;
    TcpGateway* _tcpGateway = nullptr;
    std::function<void(const std::string&)> _notificationBroadcaster;
    std::string _defaultProfile = "all";
    std::string _activeProfile;
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
