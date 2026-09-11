/*
 * McpServer.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_MCP_SERVER_HXX
#define AIMON_MCP_SERVER_HXX

#include <string>
#include "StateStore.hxx"
#include <nlohmann/json.hpp>

namespace aimon {

class McpServer {
public:
    explicit McpServer(StateStore& stateStore);

    void run();
    nlohmann::json handleMessage(const nlohmann::json& request);

private:
    nlohmann::json handleInitialize(const nlohmann::json& id, const nlohmann::json& params);
    nlohmann::json handleToolsList(const nlohmann::json& id);
    nlohmann::json handleToolsCall(const nlohmann::json& id, const nlohmann::json& params);

    std::string formatAntigravityStatus(const AntigravityStatus& ag);
    std::string formatCursorStatus(const CursorStatus& cr);
    std::string formatCombinedStatus(const AggregateStatus& status);
    std::string formatAgentTasks(const std::vector<AgentTask>& tasks);

    StateStore& _stateStore;
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
