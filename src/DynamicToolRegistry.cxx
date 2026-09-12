/*
 * DynamicToolRegistry.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "DynamicToolRegistry.hxx"
#include <iostream>

namespace aimon {

DynamicToolRegistry::DynamicToolRegistry() {
}

bool DynamicToolRegistry::registerTools(int clientId, const std::string& subsystem,
                                       const nlohmann::json& toolsArray,
                                       std::vector<std::string>& addedToolNames) {
    if (!toolsArray.is_array()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(_mutex);
    addedToolNames.clear();

    for (const auto& item : toolsArray) {
        if (!item.is_object() || !item.contains("name") || !item["name"].is_string()) {
            continue;
        }

        std::string name = item["name"].get<std::string>();
        std::string description;
        if (item.contains("description") && item["description"].is_string()) {
            description = item["description"].get<std::string>();
        }

        nlohmann::json inputSchema = nlohmann::json::object();
        if (item.contains("inputSchema") && item["inputSchema"].is_object()) {
            inputSchema = item["inputSchema"];
        }

        DynamicTool tool;
        tool.name = name;
        tool.description = description;
        tool.inputSchema = inputSchema;
        tool.subsystem = subsystem;
        tool.clientId = clientId;

        _tools[name] = tool;
        _clientTools[clientId].push_back(name);
        addedToolNames.push_back(name);
    }

    return !addedToolNames.empty();
}

bool DynamicToolRegistry::unregisterClient(int clientId, std::vector<std::string>& removedToolNames) {
    std::lock_guard<std::mutex> lock(_mutex);
    removedToolNames.clear();

    auto it = _clientTools.find(clientId);
    if (it == _clientTools.end()) {
        return false;
    }

    for (const auto& name : it->second) {
        _tools.erase(name);
        removedToolNames.push_back(name);
    }
    _clientTools.erase(it);

    return !removedToolNames.empty();
}

bool DynamicToolRegistry::hasTool(const std::string& toolName) const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _tools.find(toolName) != _tools.end();
}

bool DynamicToolRegistry::getTool(const std::string& toolName, DynamicTool& outTool) const {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _tools.find(toolName);
    if (it == _tools.end()) {
        return false;
    }
    outTool = it->second;
    return true;
}

int DynamicToolRegistry::getToolClientId(const std::string& toolName) const {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _tools.find(toolName);
    if (it == _tools.end()) {
        return -1;
    }
    return it->second.clientId;
}

nlohmann::json DynamicToolRegistry::getToolsListJson() const {
    std::lock_guard<std::mutex> lock(_mutex);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& pair : _tools) {
        const auto& t = pair.second;
        nlohmann::json item = {
            {"name", t.name},
            {"description", t.description},
            {"inputSchema", t.inputSchema}
        };
        arr.push_back(item);
    }
    return arr;
}

std::vector<DynamicTool> DynamicToolRegistry::getAllTools() const {
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<DynamicTool> result;
    result.reserve(_tools.size());
    for (const auto& pair : _tools) {
        result.push_back(pair.second);
    }
    return result;
}

size_t DynamicToolRegistry::getToolCount() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _tools.size();
}

void DynamicToolRegistry::clear() {
    std::lock_guard<std::mutex> lock(_mutex);
    _tools.clear();
    _clientTools.clear();
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
