/*
 * DynamicToolRegistry.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_DYNAMIC_TOOL_REGISTRY_HXX
#define AIMON_DYNAMIC_TOOL_REGISTRY_HXX

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>

namespace aimon {

struct DynamicTool {
    std::string name;
    std::string description;
    nlohmann::json inputSchema;
    std::string subsystem;
    int clientId = -1;
};

class DynamicToolRegistry {
public:
    DynamicToolRegistry();
    ~DynamicToolRegistry() = default;

    bool registerTools(int clientId, const std::string& subsystem,
                       const nlohmann::json& toolsArray,
                       std::vector<std::string>& addedToolNames);

    bool unregisterClient(int clientId, std::vector<std::string>& removedToolNames);

    bool hasTool(const std::string& toolName) const;
    bool getTool(const std::string& toolName, DynamicTool& outTool) const;
    int getToolClientId(const std::string& toolName) const;

    nlohmann::json getToolsListJson() const;
    std::vector<DynamicTool> getAllTools() const;
    size_t getToolCount() const;

    void clear();

private:
    mutable std::mutex _mutex;
    std::map<std::string, DynamicTool> _tools;
    std::map<int, std::vector<std::string>> _clientTools;
};

} // namespace aimon

#endif // AIMON_DYNAMIC_TOOL_REGISTRY_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
