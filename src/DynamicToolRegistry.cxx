/*
 * DynamicToolRegistry.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "DynamicToolRegistry.hxx"
#include <algorithm>
#include <iostream>

namespace aimon {

DynamicToolRegistry::DynamicToolRegistry() {
}

bool DynamicToolRegistry::registerTools(int clientId, const std::string& subsystem,
                                       const nlohmann::json& toolsArray,
                                       std::vector<std::string>& addedToolNames,
                                       const nlohmann::json& clientCapabilities) {
    if (!toolsArray.is_array()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(_mutex);
    addedToolNames.clear();

    int64_t defaultScore = 0;
    bool defaultGpu = false;
    std::string defaultDevice;

    if (clientCapabilities.is_object()) {
        if (clientCapabilities.contains("capability_score") &&
            clientCapabilities["capability_score"].is_number()) {
            defaultScore = clientCapabilities["capability_score"].get<int64_t>();
        }
        if (clientCapabilities.contains("gpu_available") &&
            clientCapabilities["gpu_available"].is_boolean()) {
            defaultGpu = clientCapabilities["gpu_available"].get<bool>();
        }
        if (clientCapabilities.contains("device_name") &&
            clientCapabilities["device_name"].is_string()) {
            defaultDevice = clientCapabilities["device_name"].get<std::string>();
        } else if (clientCapabilities.contains("compute_device") &&
                   clientCapabilities["compute_device"].is_string()) {
            defaultDevice = clientCapabilities["compute_device"].get<std::string>();
        }

        if (clientCapabilities.contains("accelerators") &&
            clientCapabilities["accelerators"].is_object()) {
            const auto& acc = clientCapabilities["accelerators"];
            if (acc.contains("capability_score") && acc["capability_score"].is_number()) {
                defaultScore = acc["capability_score"].get<int64_t>();
            }
            if (acc.contains("gpu_available") && acc["gpu_available"].is_boolean()) {
                defaultGpu = acc["gpu_available"].get<bool>();
            }
            if (acc.contains("device_name") && acc["device_name"].is_string()) {
                defaultDevice = acc["device_name"].get<std::string>();
            } else if (acc.contains("compute_device") && acc["compute_device"].is_string()) {
                defaultDevice = acc["compute_device"].get<std::string>();
            }
        }
    }

    std::vector<std::string> prevNames;
    auto prevIt = _clientTools.find(clientId);
    if (prevIt != _clientTools.end()) {
        for (const auto& t : prevIt->second) {
            prevNames.push_back(t.name);
        }
    }

    std::vector<DynamicTool> clientToolList;

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
        tool.capabilityScore = defaultScore;
        tool.gpuAccelerated = defaultGpu;
        tool.computeDevice = defaultDevice;
        tool.capabilities = clientCapabilities.is_object() ? clientCapabilities : nlohmann::json::object();

        if (item.contains("capability_score") && item["capability_score"].is_number()) {
            tool.capabilityScore = item["capability_score"].get<int64_t>();
        }
        if (item.contains("gpu_accelerated") && item["gpu_accelerated"].is_boolean()) {
            tool.gpuAccelerated = item["gpu_accelerated"].get<bool>();
        }
        if (item.contains("compute_device") && item["compute_device"].is_string()) {
            tool.computeDevice = item["compute_device"].get<std::string>();
        } else if (item.contains("device_name") && item["device_name"].is_string()) {
            tool.computeDevice = item["device_name"].get<std::string>();
        }

        if (item.contains("capabilities") && item["capabilities"].is_object()) {
            tool.capabilities = item["capabilities"];
            if (tool.capabilities.contains("capability_score") &&
                tool.capabilities["capability_score"].is_number()) {
                tool.capabilityScore = tool.capabilities["capability_score"].get<int64_t>();
            }
            if (tool.capabilities.contains("gpu_available") &&
                tool.capabilities["gpu_available"].is_boolean()) {
                tool.gpuAccelerated = tool.capabilities["gpu_available"].get<bool>();
            }
            if (tool.capabilities.contains("device_name") &&
                tool.capabilities["device_name"].is_string()) {
                tool.computeDevice = tool.capabilities["device_name"].get<std::string>();
            } else if (tool.capabilities.contains("compute_device") &&
                       tool.capabilities["compute_device"].is_string()) {
                tool.computeDevice = tool.capabilities["compute_device"].get<std::string>();
            }
        }

        clientToolList.push_back(tool);
        addedToolNames.push_back(name);
    }

    _clientTools[clientId] = std::move(clientToolList);

    for (const auto& name : addedToolNames) {
        recomputeBestToolLocked(name);
    }
    for (const auto& prevName : prevNames) {
        recomputeBestToolLocked(prevName);
    }

    return !addedToolNames.empty();
}

void DynamicToolRegistry::recomputeBestToolLocked(const std::string& toolName) {
    const DynamicTool* best = nullptr;
    for (const auto& pair : _clientTools) {
        for (const auto& tool : pair.second) {
            if (tool.name == toolName) {
                if (!best) {
                    best = &tool;
                } else if (tool.capabilityScore > best->capabilityScore) {
                    best = &tool;
                } else if (tool.capabilityScore == best->capabilityScore && tool.clientId > best->clientId) {
                    best = &tool;
                }
            }
        }
    }

    if (best) {
        _tools[toolName] = *best;
    } else {
        _tools.erase(toolName);
    }
}

bool DynamicToolRegistry::unregisterClient(int clientId, std::vector<std::string>& removedToolNames) {
    std::lock_guard<std::mutex> lock(_mutex);
    removedToolNames.clear();

    auto it = _clientTools.find(clientId);
    if (it == _clientTools.end()) {
        return false;
    }

    std::vector<DynamicTool> ownedTools = std::move(it->second);
    _clientTools.erase(it);

    for (const auto& owned : ownedTools) {
        recomputeBestToolLocked(owned.name);
        if (_tools.find(owned.name) == _tools.end()) {
            removedToolNames.push_back(owned.name);
        }
    }

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

std::vector<DynamicTool> DynamicToolRegistry::getToolCandidates(const std::string& toolName) const {
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<DynamicTool> candidates;
    for (const auto& pair : _clientTools) {
        for (const auto& tool : pair.second) {
            if (tool.name == toolName) {
                candidates.push_back(tool);
            }
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const DynamicTool& a, const DynamicTool& b) {
        if (a.capabilityScore != b.capabilityScore) {
            return a.capabilityScore > b.capabilityScore;
        }
        return a.clientId > b.clientId;
    });

    return candidates;
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
