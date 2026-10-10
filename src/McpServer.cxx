/*
 * McpServer.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "McpServer.hxx"
#include "DynamicToolRegistry.hxx"
#include "TcpGateway.hxx"
#include "TaskRegistry.hxx"
#include "ServiceSupervisor.hxx"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstdio>
#include <ctime>

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

    // Core AI quota and fleet supervisor tools are always accessible in all profiles
    if (toolName == "check_antigravity_quota" ||
        toolName == "check_cursor_usage" ||
        toolName == "check_claude_usage" ||
        toolName == "get_combined_ai_status" ||
        toolName.rfind("service_", 0) == 0) {
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

    if (profile == "mail") {
        return (toolName.rfind("mail_", 0) == 0);
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

    std::string effectiveProfile = !profile.empty() ? profile : this->_defaultProfile;

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

nlohmann::json McpServer::handleInitialize(const nlohmann::json& id, const nlohmann::json& /* params */) {
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
        {"name", "check_claude_usage"},
        {"description", "Returns estimated Claude Code usage and cost per account: tokens and est. cost over 5 hours, 7 days, the configured window and the billing cycle, per-model breakdown, plan tier, and price freshness. Figures are estimates at published list prices from local transcripts and can differ from the billing page."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", nlohmann::json::object()}
        }}
    });

    toolsArray.push_back({
        {"name", "get_combined_ai_status"},
        {"description", "Returns a comprehensive markdown summary of Google Antigravity, Cursor and Claude Code usage, quotas and limits."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", nlohmann::json::object()}
        }}
    });

    // 2. Fleet Service Supervisor Tools (available in all profiles)
    toolsArray.push_back({
        {"name", "service_list"},
        {"description", "Lists all supervised satellite services across the fleet with their current health state (HEALTHY, DEGRADED, RESTARTING, CRASH_LOOP, DISABLED), host, ports, and latency."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", nlohmann::json::object()}
        }}
    });

    toolsArray.push_back({
        {"name", "service_status"},
        {"description", "Gets detailed runtime health status, latency, failure counts, and restart history for a specific supervised service."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"service", {{"type", "string"}, {"description", "The service identifier (e.g. meshmon, netmon, embdevenv)"}}}
            }},
            {"required", nlohmann::json::array({"service"})}
        }}
    });

    toolsArray.push_back({
        {"name", "service_restart"},
        {"description", "Requests an asynchronous restart of a supervised satellite service."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"service", {{"type", "string"}, {"description", "The service identifier to restart"}}},
                {"force", {{"type", "boolean"}, {"description", "Force restart even if the service is in CRASH_LOOP state"}}}
            }},
            {"required", nlohmann::json::array({"service"})}
        }}
    });

    toolsArray.push_back({
        {"name", "service_get_logs"},
        {"description", "Retrieves trailing log lines from a supervised satellite service."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"service", {{"type", "string"}, {"description", "The service identifier"}}},
                {"lines", {{"type", "integer"}, {"description", "Number of trailing lines to fetch (default: 50)"}}}
            }},
            {"required", nlohmann::json::array({"service"})}
        }}
    });

    // 3. Dynamic Satellite Tools filtered by active profile
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
    } else if (toolName == "check_claude_usage") {
        contentText = formatClaudeStatus(current.claude);
    } else if (toolName == "get_combined_ai_status") {
        contentText = formatCombinedStatus(current);
    } else if (toolName == "service_list") {
        if (!_serviceSupervisor) {
            contentText = "Fleet supervisor is not enabled or not running.";
        } else {
            auto statuses = _serviceSupervisor->getAllStatuses();
            std::ostringstream oss;
            oss << "### Fleet Satellite Daemons (" << statuses.size() << " monitored)\n\n"
                << "| Service | Host | Port | Status | Latency | Restarts | Last Probe |\n"
                << "|---------|------|------|--------|---------|----------|------------|\n";
            for (const auto& s : statuses) {
                oss << "| " << s.config.name << " (`" << s.config.id << "`) | "
                    << s.config.host << " | " << s.config.port << " | "
                    << serviceStateToString(s.state) << " | "
                    << s.probeLatencyMs << " ms | "
                    << s.restartCount << " | "
                    << s.lastProbeEpoch << " |\n";
            }
            contentText = oss.str();
        }
    } else if (toolName == "service_status") {
        nlohmann::json args = params.value("arguments", nlohmann::json::object());
        std::string serviceId = args.value("service", args.value("id", ""));
        if (serviceId.empty()) {
            return {
                {"jsonrpc", "2.0"},
                {"id", id},
                {"error", {{"code", -32602}, {"message", "Missing required argument 'service'"}}}
            };
        }
        if (!_serviceSupervisor) {
            contentText = "Fleet supervisor is not enabled or not running.";
        } else {
            ServiceRuntimeStatus status;
            if (!_serviceSupervisor->getStatus(serviceId, status)) {
                return {
                    {"jsonrpc", "2.0"},
                    {"id", id},
                    {"error", {{"code", -32602}, {"message", "Unknown service: " + serviceId}}}
                };
            }
            contentText = status.toJson().dump(2);
        }
    } else if (toolName == "service_restart") {
        nlohmann::json args = params.value("arguments", nlohmann::json::object());
        std::string serviceId = args.value("service", args.value("id", ""));
        bool force = args.value("force", false);
        if (serviceId.empty()) {
            return {
                {"jsonrpc", "2.0"},
                {"id", id},
                {"error", {{"code", -32602}, {"message", "Missing required argument 'service'"}}}
            };
        }
        if (!_serviceSupervisor) {
            return {
                {"jsonrpc", "2.0"},
                {"id", id},
                {"error", {{"code", -32603}, {"message", "Fleet supervisor is not running"}}}
            };
        }
        bool ok = _serviceSupervisor->requestRestart(serviceId, force);
        if (!ok) {
            contentText = "Failed to restart service '" + serviceId + "' (not found or anti-thrashing circuit breaker active).";
        } else {
            contentText = "Successfully dispatched restart for service '" + serviceId + "'.";
        }
    } else if (toolName == "service_get_logs") {
        nlohmann::json args = params.value("arguments", nlohmann::json::object());
        std::string serviceId = args.value("service", args.value("id", ""));
        int lines = args.value("lines", 50);
        if (serviceId.empty()) {
            return {
                {"jsonrpc", "2.0"},
                {"id", id},
                {"error", {{"code", -32602}, {"message", "Missing required argument 'service'"}}}
            };
        }
        if (!_serviceSupervisor) {
            contentText = "Fleet supervisor is not running.";
        } else {
            contentText = _serviceSupervisor->getServiceLogs(serviceId, lines);
            if (contentText.empty()) {
                contentText = "(No log output retrieved for " + serviceId + ")";
            }
        }
    } else if (_dynamicRegistry && _tcpGateway && _dynamicRegistry->hasTool(toolName)) {
        nlohmann::json args = params.value("arguments", nlohmann::json::object());
        nlohmann::json gwResult;
        std::string gwError;

        int timeoutMs = 45000;
        if (args.contains("timeout_sec") && args["timeout_sec"].is_number()) {
            timeoutMs = (args["timeout_sec"].get<int>() + 10) * 1000;
        } else if (toolName.find("flash") != std::string::npos) {
            timeoutMs = 180000;
        }

        bool ok = _tcpGateway->callTool(toolName, args, gwResult, gwError, timeoutMs);
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
    ss << "\n---\n\n";
    ss << formatClaudeStatus(status.claude);
    return ss.str();
}

namespace {

// Dollars from integer nano-dollars, rounded half up to the cent.
std::string claudeUsd(int64_t nano) {
    if (nano < 0) {
        nano = 0;
    }
    const int64_t cents = (nano + 5000000) / 10000000;
    char buf[48];
    std::snprintf(buf, sizeof(buf), "$%lld.%02lld", static_cast<long long>(cents / 100),
                  static_cast<long long>(cents % 100));
    return buf;
}

std::string claudeTokens(int64_t n) {
    char buf[48];
    if (n < 1000) {
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(n));
    } else if (n < 1000000) {
        std::snprintf(buf, sizeof(buf), "%.1fK", static_cast<double>(n) / 1e3);
    } else if (n < 1000000000) {
        std::snprintf(buf, sizeof(buf), "%.2fM", static_cast<double>(n) / 1e6);
    } else {
        std::snprintf(buf, sizeof(buf), "%.2fB", static_cast<double>(n) / 1e9);
    }
    return buf;
}

// Names come from files and config; keep them from breaking markdown tables.
std::string claudeMdSafe(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '|' || c == '`' || c == '\n' || c == '\r') {
            out.push_back('_');
        } else {
            out.push_back(c);
        }
        if (out.size() >= 80) {
            break;
        }
    }
    return out;
}

std::string claudeUtc(int64_t epoch) {
    time_t t = static_cast<time_t>(epoch);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    char buf[48];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M UTC", &tmv);
    return buf;
}

} // namespace

std::string McpServer::formatClaudeStatus(const ClaudeStatus& cl) {
    std::ostringstream ss;
    ss << "### Claude Code Usage (estimated)\n\n";
    if (!cl.enabled) {
        ss << "Claude usage collection is disabled.\n";
        return ss.str();
    }
    if (cl.accounts.empty()) {
        ss << "No Claude accounts are configured.\n";
        return ss.str();
    }
    ss << "_Estimates from Claude Code transcripts on this host at published list prices. "
       << "All dollar figures are est. values, not invoices, and can differ from the billing page "
       << "(price changes, other machines, other products)._\n";

    for (const ClaudeAccountStatus& a : cl.accounts) {
        const char* tierName = a.tier == ClaudeTier::Enterprise ? "Enterprise"
                             : a.tier == ClaudeTier::Personal ? "Personal" : "Unknown tier";
        const char* valueLabel = a.tier == ClaudeTier::Enterprise ? "Est. cost"
                               : a.tier == ClaudeTier::Personal ? "Est. API-equivalent value" : "Est. value";
        ss << "\n#### Account `" << claudeMdSafe(a.name) << "` \xE2\x80\x94 " << tierName;
        if (!a.rawSubscriptionType.empty()) {
            ss << " (" << claudeMdSafe(a.rawSubscriptionType);
            if (!a.rawRateLimitTier.empty()) {
                ss << " / " << claudeMdSafe(a.rawRateLimitTier);
            }
            ss << ")";
        }
        ss << "\n";

        if (!a.hasData) {
            ss << "- **Status**: no Claude Code usage found\n";
        } else {
            if (a.cycleConfigured) {
                ss << "- **" << valueLabel << " this cycle**: " << claudeUsd(a.cycle.costNano);
                if (a.spendLimitUsd > 0.0) {
                    ss << " of $" << std::fixed << std::setprecision(2) << a.spendLimitUsd << " ("
                       << std::setprecision(1) << a.estPctOfLimit << "%)";
                }
                ss << ", cycle resets " << a.cycleResetIso << "\n";
            }
            ss << "- **" << valueLabel << "**: " << claudeUsd(a.last5h.costNano) << " (5 h) \xC2\xB7 "
               << claudeUsd(a.last7d.costNano) << " (7 d) \xC2\xB7 " << claudeUsd(a.window.costNano)
               << " (" << a.windowDays << " d)\n";
            ss << "- **Tokens**: " << claudeTokens(a.last5h.totalTokens()) << " (5 h) \xC2\xB7 "
               << claudeTokens(a.last7d.totalTokens()) << " (7 d) \xC2\xB7 "
               << claudeTokens(a.window.totalTokens()) << " (" << a.windowDays << " d)\n";
            ss << "- **Messages (" << a.windowDays << " d)**: " << a.window.messages << "\n";

            if (!a.models.empty()) {
                ss << "\n| Model | Messages | Tokens | Est. cost |\n|---|---:|---:|---:|\n";
                size_t shown = 0;
                for (const ClaudeModelUsage& m : a.models) {
                    if (++shown > 10) {
                        break;
                    }
                    const int64_t tokens = m.input + m.output + m.cacheRead + m.cacheWrite5m + m.cacheWrite1h;
                    ss << "| `" << claudeMdSafe(m.model) << "` | " << m.messages << " | " << claudeTokens(tokens)
                       << " | " << claudeUsd(m.costNano) << " |\n";
                }
                ss << "\n";
            }
            if (!a.unpricedModels.empty()) {
                ss << "- **Unpriced models**: ";
                for (size_t i = 0; i < a.unpricedModels.size(); ++i) {
                    ss << (i ? ", " : "") << "`" << claudeMdSafe(a.unpricedModels[i]) << "`";
                }
                ss << " (tokens counted, no cost)\n";
            }
        }
        if (a.skippedLines > 0) {
            ss << "- **Skipped**: " << a.skippedLines << " lines skipped (malformed or out of range)\n";
        }
        if (!a.errorMessage.empty()) {
            ss << "- **Error**: " << claudeMdSafe(a.errorMessage) << "\n";
        }
        if (!a.warning.empty()) {
            ss << "- **Warning**: " << claudeMdSafe(a.warning) << "\n";
        }
    }

    ss << "\n- **Prices**: ";
    if (cl.pricing.fetchedAtEpoch > 0) {
        ss << cl.pricing.modelsLoaded << " models, as of " << claudeUtc(cl.pricing.fetchedAtEpoch);
        if (cl.pricing.stale) {
            ss << " \xE2\x80\x94 STALE (no successful fetch for over 3 refresh intervals)";
        }
        if (cl.pricing.quarantinedCount > 0) {
            ss << "; " << cl.pricing.quarantinedCount << " models quarantined (left unpriced)";
        }
    } else {
        ss << "none loaded";
    }
    ss << "\n";
    if (!cl.pricing.error.empty()) {
        ss << "- **Price fetch**: " << claudeMdSafe(cl.pricing.error) << "\n";
    }
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
