/*
 * McpServer.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "McpServer.hxx"
#include "TaskRegistry.hxx"
#include <iostream>
#include <sstream>
#include <iomanip>

namespace aimon {

McpServer::McpServer(StateStore& stateStore)
    : _stateStore(stateStore) {
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

nlohmann::json McpServer::handleMessage(const nlohmann::json& request) {
    if (!request.contains("method")) {
        return nullptr;
    }

    std::string method = request["method"].get<std::string>();
    nlohmann::json id = request.value("id", nlohmann::json());

    if (method == "initialize") {
        return handleInitialize(id, request.value("params", nlohmann::json::object()));
    } else if (method == "notifications/initialized") {
        // Notification, no response
        return nullptr;
    } else if (method == "ping") {
        return {
            {"jsonrpc", "2.0"},
            {"id", id},
            {"result", nlohmann::json::object()}
        };
    } else if (method == "tools/list") {
        return handleToolsList(id);
    } else if (method == "tools/call") {
        return handleToolsCall(id, request.value("params", nlohmann::json::object()));
    }

    // Unhandled method
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
                {"version", "1.0.0"}
            }}
        }}
    };
}

nlohmann::json McpServer::handleToolsList(const nlohmann::json& id) {
    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", {
            {"tools", {
                {
                    {"name", "check_antigravity_quota"},
                    {"description", "Returns Google Antigravity remaining capacity %, prompt credits, flow credits, and model reset timestamps."},
                    {"inputSchema", {
                        {"type", "object"},
                        {"properties", nlohmann::json::object()}
                    }}
                },
                {
                    {"name", "check_cursor_usage"},
                    {"description", "Returns Cursor fast requests used vs limit, plan tier, and billing cycle reset date."},
                    {"inputSchema", {
                        {"type", "object"},
                        {"properties", nlohmann::json::object()}
                    }}
                },
                {
                    {"name", "get_combined_ai_status"},
                    {"description", "Returns a comprehensive markdown summary of both Google Antigravity and Cursor AI quotas and limits."},
                    {"inputSchema", {
                        {"type", "object"},
                        {"properties", nlohmann::json::object()}
                    }}
                },
                {
                    {"name", "register_agent_task"},
                    {"description", "Registers or updates an active agent task and heartbeat in aimon's task registry."},
                    {"inputSchema", {
                        {"type", "object"},
                        {"properties", {
                            {"task_id", {
                                {"type", "string"},
                                {"description", "Optional unique task identifier. Generated if omitted."}
                            }},
                            {"agent_name", {
                                {"type", "string"},
                                {"description", "Name of the agent (e.g. 'Cursor (Windows)', 'Antigravity IDE', 'CLI Worker')."}
                            }},
                            {"workspace", {
                                {"type", "string"},
                                {"description", "Workspace directory or project name."}
                            }},
                            {"task_description", {
                                {"type", "string"},
                                {"description", "User goal or task prompt being executed."}
                            }},
                            {"current_action", {
                                {"type", "string"},
                                {"description", "Currently active sub-step, tool call, or reasoning action."}
                            }},
                            {"status", {
                                {"type", "string"},
                                {"description", "Status of the task ('running', 'waiting_for_user', 'completed', 'failed')."},
                                {"enum", {"running", "waiting_for_user", "completed", "failed"}}
                            }},
                            {"details", {
                                {"type", "string"},
                                {"description", "Optional extra details or output summary."}
                            }}
                        }},
                        {"required", {"task_description"}}
                    }}
                },
                {
                    {"name", "list_active_tasks"},
                    {"description", "Lists all active AI agent tasks, their current actions, and runtimes across the workspace fleet."},
                    {"inputSchema", {
                        {"type", "object"},
                        {"properties", {
                            {"include_completed", {
                                {"type", "boolean"},
                                {"description", "Whether to include completed/failed tasks (default: false)."}
                            }}
                        }}
                    }}
                }
            }}
        }}
    };
}

nlohmann::json McpServer::handleToolsCall(const nlohmann::json& id, const nlohmann::json& params) {
    std::string toolName = params.value("name", "");
    AggregateStatus current = _stateStore.getStatus();
    std::string contentText;

    if (toolName == "check_antigravity_quota") {
        contentText = formatAntigravityStatus(current.antigravity);
    } else if (toolName == "check_cursor_usage") {
        contentText = formatCursorStatus(current.cursor);
    } else if (toolName == "get_combined_ai_status") {
        contentText = formatCombinedStatus(current);
    } else if (toolName == "register_agent_task") {
        nlohmann::json args = params.value("arguments", nlohmann::json::object());
        AgentTask task;
        task.taskId = args.value("task_id", "");
        task.agentName = args.value("agent_name", "Unknown Agent");
        task.workspace = args.value("workspace", "");
        task.taskDescription = args.value("task_description", "");
        task.currentAction = args.value("current_action", "");
        task.status = args.value("status", "running");
        task.details = args.value("details", "");
        task.sseSessionId = args.value("sse_session_id", "");

        std::string registeredId = TaskRegistry::getInstance().registerOrUpdateTask(task);
        AgentTask savedTask;
        TaskRegistry::getInstance().getTask(registeredId, savedTask);

        std::ostringstream oss;
        oss << "### Agent Task Registered\n\n"
            << "- **Task ID**: `" << registeredId << "`\n"
            << "- **Agent**: " << savedTask.agentName << "\n"
            << "- **Status**: " << savedTask.status << "\n"
            << "- **Task Description**: " << savedTask.taskDescription << "\n";
        if (!savedTask.currentAction.empty()) {
            oss << "- **Current Action**: `" << savedTask.currentAction << "`\n";
        }
        if (!savedTask.workspace.empty()) {
            oss << "- **Workspace**: `" << savedTask.workspace << "`\n";
        }
        contentText = oss.str();
    } else if (toolName == "list_active_tasks") {
        nlohmann::json args = params.value("arguments", nlohmann::json::object());
        bool includeCompleted = args.value("include_completed", false);
        auto tasks = TaskRegistry::getInstance().listTasks(includeCompleted);
        contentText = formatAgentTasks(tasks);
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

    ss << "- **Plan Tier**: " << ag.planTier << "\n\n";

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

std::string McpServer::formatAgentTasks(const std::vector<AgentTask>& tasks) {
    std::ostringstream oss;
    oss << "### Active AI Agents Fleet (" << tasks.size() << " recorded)\n\n";

    if (tasks.empty()) {
        oss << "_No active agent tasks currently registered._\n\n"
            << "Agents can register tasks using the `register_agent_task` tool.\n";
        return oss.str();
    }

    oss << "| Task ID | Agent | Status | Duration | Task Goal | Active Action | Last Seen |\n"
        << "| :--- | :--- | :--- | :--- | :--- | :--- | :--- |\n";

    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    for (const auto& t : tasks) {
        std::string statusIcon = "🟢";
        if (t.status == "waiting_for_user") statusIcon = "🟡";
        else if (t.status == "completed") statusIcon = "🔵";
        else if (t.status == "failed") statusIcon = "🔴";
        else if (t.status == "stale" || t.status == "disconnected") statusIcon = "⚪";

        int64_t durationSec = (t.completedTimeEpoch > 0 ? t.completedTimeEpoch : now) - t.startTimeEpoch;
        if (durationSec < 0) durationSec = 0;
        int min = durationSec / 60;
        int sec = durationSec % 60;
        std::ostringstream durOss;
        durOss << std::setw(2) << std::setfill('0') << min << "m "
               << std::setw(2) << std::setfill('0') << sec << "s";

        int64_t lastSeenSec = now - t.lastHeartbeatEpoch;
        if (lastSeenSec < 0) lastSeenSec = 0;
        std::string lastSeenStr;
        if (lastSeenSec < 60) {
            lastSeenStr = std::to_string(lastSeenSec) + "s ago";
        } else {
            lastSeenStr = std::to_string(lastSeenSec / 60) + "m ago";
        }

        std::string actionStr = t.currentAction.empty() ? "-" : ("`" + t.currentAction + "`");

        oss << "| `" << t.taskId << "` "
            << "| " << t.agentName << " "
            << "| " << statusIcon << " " << t.status << " "
            << "| " << durOss.str() << " "
            << "| " << t.taskDescription << " "
            << "| " << actionStr << " "
            << "| " << lastSeenStr << " |\n";
    }

    return oss.str();
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
