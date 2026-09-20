/*
 * McpServer.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "McpServer.hxx"
#include "DynamicToolRegistry.hxx"
#include "TcpGateway.hxx"
#include "TaskRegistry.hxx"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace aimon {

McpServer::McpServer(StateStore& stateStore,
                     DynamicToolRegistry* dynamicRegistry,
                     TcpGateway* tcpGateway)
    : _stateStore(stateStore),
      _dynamicRegistry(dynamicRegistry),
      _tcpGateway(tcpGateway),
      _defaultProfile("all") {
}

void McpServer::notifyToolsListChanged() {
    if (_notificationBroadcaster) {
        nlohmann::json notif = {
            {"jsonrpc", "2.0"},
            {"method", "notifications/tools/list_changed"},
            {"params", nlohmann::json::object()}
        };
        _notificationBroadcaster(notif.dump());
    }
}

bool McpServer::isToolAllowedInProfile(const std::string& toolName, const std::string& profile) {
    if (profile.empty() || profile == "all") {
        return true;
    }

    // Core AI quota tools are always accessible in all profiles
    if (toolName == "check_antigravity_quota" ||
        toolName == "check_cursor_usage" ||
        toolName == "get_combined_ai_status") {
        return true;
    }

    if (profile == "core") {
        return false;
    }

    if (profile == "embedded") {
        return (toolName.rfind("embdevenv_", 0) == 0);
    }

    if (profile == "network") {
        return (toolName.rfind("lan_", 0) == 0 ||
                toolName.rfind("firewall_", 0) == 0 ||
                toolName.rfind("snmp_", 0) == 0);
    }

    if (profile == "mesh") {
        return (toolName.rfind("meshmon_", 0) == 0);
    }

    return true;
}

std::string McpServer::detectProfileFromWorkspace(const std::string& ws) {
    if (ws.empty()) {
        return "";
    }

    std::string lower = ws;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower.find("amba-virt") != std::string::npos ||
        lower.find("robohero") != std::string::npos ||
        lower.find("embdevenv") != std::string::npos) {
        return "embedded";
    }

    if (lower.find("mesh") != std::string::npos) {
        return "mesh";
    }

    if (lower.find("netmon") != std::string::npos) {
        return "network";
    }

    if (lower.find("intelligence") != std::string::npos ||
        lower.find("aimon") != std::string::npos) {
        return "core";
    }

    return "";
}

void McpServer::run() {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;

        try {
            nlohmann::json request = nlohmann::json::parse(line, nullptr, false);
            if (request.is_discarded()) {
                std::cerr << "[McpServer] Warning: malformed JSON input: " << line << std::endl;
                continue;
            }

            nlohmann::json response = handleMessage(request);
            if (!response.is_null()) {
                std::cout << response.dump() << "\n";
                std::cout.flush();
            }
        } catch (const std::exception& e) {
            std::cerr << "[McpServer] Exception handling request: " << e.what() << std::endl;
        }
    }
}

nlohmann::json McpServer::handleMessage(const nlohmann::json& request, const std::string& profile) {
    if (!request.contains("method")) {
        return nullptr;
    }

    std::string method = request["method"].get<std::string>();
    nlohmann::json id = request.value("id", nlohmann::json());

    std::string effectiveProfile = !profile.empty() ? profile : (!this->_activeProfile.empty() ? this->_activeProfile : this->_defaultProfile);

    if (method == "initialize") {
        return handleInitialize(id, request.value("params", nlohmann::json::object()));
    } else if (method == "notifications/initialized") {
        return nullptr;
    } else if (method == "ping") {
        return {
            {"jsonrpc", "2.0"},
            {"id", id},
            {"result", nlohmann::json::object()}
        };
    } else if (method == "tools/list") {
        return handleToolsList(id, effectiveProfile);
    } else if (method == "tools/call") {
        return handleToolsCall(id, request.value("params", nlohmann::json::object()), effectiveProfile);
    }

    if (!id.is_null()) {
        return {
            {"jsonrpc", "2.0"},
            {"id", id},
            {"error", {
                {"code", -32601},
                {"message", "Method not found: " + method}
            }}
        };
    }

    return nullptr;
}

nlohmann::json McpServer::handleInitialize(const nlohmann::json& id, const nlohmann::json& params) {
    // Attempt auto-scoping based on client workspace if active profile is not explicitly overridden
    if (_activeProfile.empty() || _activeProfile == "all") {
        std::string detected;
        if (params.contains("rootUri") && params["rootUri"].is_string()) {
            detected = detectProfileFromWorkspace(params["rootUri"].get<std::string>());
        }
        if (detected.empty() && params.contains("rootPath") && params["rootPath"].is_string()) {
            detected = detectProfileFromWorkspace(params["rootPath"].get<std::string>());
        }
        if (detected.empty() && params.contains("workspaceFolders") && params["workspaceFolders"].is_array()) {
            for (const auto& wf : params["workspaceFolders"]) {
                if (wf.contains("uri") && wf["uri"].is_string()) {
                    detected = detectProfileFromWorkspace(wf["uri"].get<std::string>());
                    if (!detected.empty()) break;
                }
            }
        }
        if (!detected.empty()) {
            _activeProfile = detected;
            std::cerr << "[McpServer] Auto-scoped MCP tool profile to: " << _activeProfile << std::endl;
        }
    }

    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", {
            {"protocolVersion", "2024-11-05"},
            {"capabilities", {
                {"tools", nlohmann::json::object()}
            }},
            {"serverInfo", {
                {"name", "aimon"},
                {"version", "1.1.0"}
            }}
        }}
    };
}

nlohmann::json McpServer::handleToolsList(const nlohmann::json& id, const std::string& profile) {
    nlohmann::json toolsArray = nlohmann::json::array();

    // 1. Core AI Quota Tools (always included)
    toolsArray.push_back({
        {"name", "check_antigravity_quota"},
        {"description", "Returns Google Antigravity remaining capacity %, prompt credits, flow credits, and model reset timestamps."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", nlohmann::json::object()}
        }}
    });

    toolsArray.push_back({
        {"name", "check_cursor_usage"},
        {"description", "Returns Cursor fast requests used vs limit, plan tier, and billing cycle reset date."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", nlohmann::json::object()}
        }}
    });

    toolsArray.push_back({
        {"name", "get_combined_ai_status"},
        {"description", "Returns a comprehensive markdown summary of both Google Antigravity and Cursor AI quotas and limits."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", nlohmann::json::object()}
        }}
    });

    // 2. Dynamic Satellite Tools filtered by active profile
    if (profile != "core" && _dynamicRegistry) {
        nlohmann::json dynTools = _dynamicRegistry->getToolsListJson();
        if (dynTools.is_array()) {
            for (const auto& dt : dynTools) {
                if (dt.contains("name") && dt["name"].is_string()) {
                    std::string tName = dt["name"].get<std::string>();
                    if (isToolAllowedInProfile(tName, profile)) {
                        toolsArray.push_back(dt);
                    }
                }
            }
        }
    }

    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", {
            {"tools", toolsArray}
        }}
    };
}

nlohmann::json McpServer::handleToolsCall(const nlohmann::json& id, const nlohmann::json& params, const std::string& profile) {
    std::string toolName = params.value("name", "");

    if (!isToolAllowedInProfile(toolName, profile)) {
        return {
            {"jsonrpc", "2.0"},
            {"id", id},
            {"error", {
                {"code", -32601},
                {"message", "Tool '" + toolName + "' is not available under active profile '" + profile + "'"}
            }}
        };
    }

    AggregateStatus current = _stateStore.getStatus();
    std::string contentText;

    if (toolName == "check_antigravity_quota") {
        contentText = formatAntigravityStatus(current.antigravity);
    } else if (toolName == "check_cursor_usage") {
        contentText = formatCursorStatus(current.cursor);
    } else if (toolName == "get_combined_ai_status") {
        contentText = formatCombinedStatus(current);
    } else if (_dynamicRegistry && _tcpGateway && _dynamicRegistry->hasTool(toolName)) {
        nlohmann::json args = params.value("arguments", nlohmann::json::object());
        nlohmann::json gwResult;
        std::string gwError;
        bool ok = _tcpGateway->callTool(toolName, args, gwResult, gwError);
        if (!ok) {
            return {
                {"jsonrpc", "2.0"},
                {"id", id},
                {"error", {
                    {"code", -32603},
                    {"message", "Subsystem tool call failed: " + gwError}
                }}
            };
        }
        if (gwResult.contains("content")) {
            return {
                {"jsonrpc", "2.0"},
                {"id", id},
                {"result", gwResult}
            };
        }
        contentText = gwResult.is_string() ? gwResult.get<std::string>() : gwResult.dump(2);
    } else {
        return {
            {"jsonrpc", "2.0"},
            {"id", id},
            {"error", {
                {"code", -32602},
                {"message", "Unknown tool: " + toolName}
            }}
        };
    }

    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", {
            {"content", {
                {
                    {"type", "text"},
                    {"text", contentText}
                }
            }}
        }}
    };
}

std::string McpServer::formatAntigravityStatus(const AntigravityStatus& ag) {
    std::ostringstream ss;
    ss << "### Google Antigravity Quota Status\n\n";

    if (!ag.isRunning) {
        ss << "**Status**: Offline\n";
        if (!ag.errorMessage.empty()) {
            ss << "**Note**: " << ag.errorMessage << "\n";
        }
        return ss.str();
    }

    ss << "- **Plan Tier**: " << ag.planTier << "\n";

    if (!ag.availableCredits.empty()) {
        for (const auto& uc : ag.availableCredits) {
            std::string typeDisplay = uc.creditType;
            if (typeDisplay == "GOOGLE_ONE_AI") {
                typeDisplay = "Google One AI credits";
            }
            ss << "- **Available Credits**: **" << uc.creditAmount << "** " << typeDisplay;
            if (uc.minimumCreditAmountForUsage > 0) {
                ss << " (minimum " << uc.minimumCreditAmountForUsage << " per request)";
            }
            ss << "\n";
        }
    }

    if (ag.availablePromptCredits > 0 || ag.availableFlowCredits > 0 ||
        ag.monthlyPromptCredits > 0 || ag.monthlyFlowCredits > 0) {
        ss << "- **Prompt / Flow Credits**: " << ag.availablePromptCredits << " prompt / "
           << ag.availableFlowCredits << " flow available";
        if (ag.monthlyPromptCredits > 0 || ag.monthlyFlowCredits > 0) {
            ss << " (Monthly allocation: " << ag.monthlyPromptCredits << " / "
               << ag.monthlyFlowCredits << ")";
        }
        ss << "\n";
    }
    ss << "\n";

    if (!ag.quotaGroups.empty()) {
        for (const auto& g : ag.quotaGroups) {
            ss << "#### " << g.displayName << "\n";
            if (!g.description.empty()) {
                ss << "*" << g.description << "*\n\n";
            }
            ss << "| Limit Window | Remaining | Reset Time |\n";
            ss << "| :--- | :--- | :--- |\n";
            for (const auto& b : g.buckets) {
                int pct = static_cast<int>(b.remainingFraction * 100.0 + 0.5);
                ss << "| " << b.displayName << " | **" << pct << "%** | "
                   << (b.resetTimeIso.empty() ? "N/A" : b.resetTimeIso) << " |\n";
            }
            ss << "\n";
        }
    } else if (!ag.models.empty()) {
        ss << "| Model | Remaining Capacity | Reset Time |\n";
        ss << "| :--- | :--- | :--- |\n";
        for (const auto& m : ag.models) {
            int pct = static_cast<int>(m.remainingFraction * 100.0 + 0.5);
            ss << "| " << m.modelName << " | " << pct << "% | " << (m.resetTimeIso.empty() ? "N/A" : m.resetTimeIso) << " |\n";
        }
    }

    return ss.str();
}

std::string McpServer::formatCursorStatus(const CursorStatus& cr) {
    std::ostringstream ss;
    ss << "### Cursor Usage & Quota Status\n\n";

    if (!cr.isAuthenticated) {
        ss << "**Status**: Unauthenticated / Free\n";
        if (!cr.planTier.empty()) {
            ss << "- **Tier**: " << cr.planTier << "\n";
        }
        if (!cr.errorMessage.empty()) {
            ss << "- **Note**: " << cr.errorMessage << "\n";
        }
        return ss.str();
    }

    ss << "- **Plan Tier**: " << cr.planTier << "\n";
    ss << "- **Fast Requests Used**: " << cr.fastRequestsUsed;
    if (cr.fastRequestsLimit > 0) {
        ss << " / " << cr.fastRequestsLimit;
        int remaining = cr.fastRequestsLimit - cr.fastRequestsUsed;
        ss << " (" << remaining << " remaining)";
    }
    ss << "\n";
    if (cr.totalSpendUsd > 0.0) {
        ss << "- **Current Billing Cycle Spend**: $" << std::fixed << std::setprecision(2) << cr.totalSpendUsd;
        if (cr.prevCycleSpendUsd > 0.0) {
            ss << " (vs $" << std::fixed << std::setprecision(2) << cr.prevCycleSpendUsd << " previous cycle)";
        }
        ss << "\n";
    }
    if (!cr.cycleResetIso.empty()) {
        ss << "- **Billing Cycle Reset**: " << cr.cycleResetIso << "\n";
    }

    if (!cr.categorySpendList.empty()) {
        ss << "\n**Personal Usage by Model**:\n";
        for (const auto& cat : cr.categorySpendList) {
            if (cat.spendUsd <= 0.0) continue;
            ss << "  - `" << cat.category << "`: $" << std::fixed << std::setprecision(2)
               << cat.spendUsd << " (" << std::fixed << std::setprecision(1) << cat.percentage << "%)\n";
        }
    }

    return ss.str();
}

std::string McpServer::formatCombinedStatus(const AggregateStatus& status) {
    std::ostringstream ss;
    ss << "# AI Subscription & Quota Summary\n\n";
    ss << formatAntigravityStatus(status.antigravity);
    ss << "\n---\n\n";
    ss << formatCursorStatus(status.cursor);
    return ss.str();
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
