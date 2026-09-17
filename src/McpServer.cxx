/*
 * McpServer.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "McpServer.hxx"
#include "DynamicToolRegistry.hxx"
#include "TcpGateway.hxx"
#include "TaskRegistry.hxx"
#include "AgentMessageBus.hxx"
#include "TranscriptSink.hxx"
#include "InterlockManager.hxx"
#include <iostream>
#include <sstream>
#include <iomanip>

namespace aimon {

McpServer::McpServer(StateStore& stateStore,
                     DynamicToolRegistry* dynamicRegistry,
                     TcpGateway* tcpGateway)
    : _stateStore(stateStore),
      _dynamicRegistry(dynamicRegistry),
      _tcpGateway(tcpGateway) {
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
    nlohmann::json resp = {
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
                },
                {
                    {"name", "agent_check_inbox"},
                    {"description", "Polls the aimon operator console for pending messages or instructions sent to this agent. Waits indefinitely if timeout_seconds is 0."},
                    {"inputSchema", {
                        {"type", "object"},
                        {"properties", {
                            {"timeout_seconds", {
                                {"type", "integer"},
                                {"description", "Maximum time in seconds to wait for a message before returning keep-alive (0 for indefinite blocking, default: 0)."}
                            }}
                        }}
                    }}
                },
                {
                    {"name", "agent_send_reply"},
                    {"description", "Delivers a reply back to the aimon operator console for a previously received message."},
                    {"inputSchema", {
                        {"type", "object"},
                        {"properties", {
                            {"message_id", {
                                {"type", "string"},
                                {"description", "The ID of the message being replied to."}
                            }},
                            {"reply_text", {
                                {"type", "string"},
                                {"description", "The text response to deliver back to the aimon operator console."}
                            }}
                        }},
                        {"required", {"message_id", "reply_text"}}
                    }}
                },
                {
                    {"name", "agent_collaborate"},
                    {"description", "Signals completion of an agent's turn in a collaborative plan document and notifies the peer agent."},
                    {"inputSchema", {
                        {"type", "object"},
                        {"properties", {
                            {"plan_file", {
                                {"type", "string"},
                                {"description", "Path to the plan file on disk (e.g. 'plan/plan_new_feature.md')."}
                            }},
                            {"turn_number", {
                                {"type", "integer"},
                                {"description", "The turn number just completed (e.g. 1, 2, 3...)."}
                            }},
                            {"agent_id", {
                                {"type", "string"},
                                {"description", "Canonical ID of calling agent ('agent-antigravity-builder' or 'agent-cursor-windows')."}
                            }},
                            {"agent_model", {
                                {"type", "string"},
                                {"description", "Model identifier string (e.g. 'Gemini 3.8 Flash High', 'Cursor Grok 4.6 High')."}
                            }},
                            {"status", {
                                {"type", "string"},
                                {"enum", {"IN_PROGRESS", "CONSENSUS_REACHED"}},
                                {"description", "Session status ('IN_PROGRESS' or 'CONSENSUS_REACHED')."}
                            }},
                            {"side_channel_message", {
                                {"type", "string"},
                                {"description", "Optional brief note or highlight passed out-of-band to the peer and displayed on operator console."}
                            }}
                        }},
                        {"required", {"plan_file", "turn_number", "agent_id", "agent_model", "status"}}
                    }}
                },
                {
                    {"name", "agent_wait_turn"},
                    {"description", "Waits or polls for this agent's turn to execute in an active plan collaboration session. Does not consume or pop the latch on read."},
                    {"inputSchema", {
                        {"type", "object"},
                        {"properties", {
                            {"plan_file", {
                                {"type", "string"},
                                {"description", "Path to the plan file to wait for (e.g. 'plan/plan_new_feature.md')."}
                            }},
                            {"agent_id", {
                                {"type", "string"},
                                {"description", "Canonical ID of calling agent ('agent-antigravity-builder' or 'agent-cursor-windows')."}
                            }},
                            {"timeout_seconds", {
                                {"type", "integer"},
                                {"description", "Seconds to wait. 0 for indefinite blocking (kernel CV wait). >0 for short polling (default: 0)."}
                            }}
                        }},
                        {"required", {"plan_file", "agent_id"}}
                    }}
                },
                {
                    {"name", "exec_start_run"},
                    {"description", "Starts a new plan execution run and initializes the transcript sink."},
                    {"inputSchema", {
                        {"type", "object"},
                        {"properties", {
                            {"plan_file", {
                                {"type", "string"},
                                {"description", "Path to plan document being executed."}
                            }},
                            {"workspace", {
                                {"type", "string"},
                                {"description", "Target workspace path or name."}
                            }},
                            {"target_alias", {
                                {"type", "string"},
                                {"description", "Hardware device or environment alias (e.g. 'n1-655-devkit')."}
                            }},
                            {"executor_id", {
                                {"type", "string"},
                                {"description", "Canonical ID of executor agent (default: 'agent-antigravity-builder')."}
                            }},
                            {"initiator_id", {
                                {"type", "string"},
                                {"description", "Canonical ID of initiator/reviewer agent (default: 'agent-cursor-windows')."}
                            }}
                        }},
                        {"required", {"plan_file", "target_alias"}}
                    }}
                },
                {
                    {"name", "exec_submit_checkpoint"},
                    {"description", "Records the result of an executed checkpoint in the current run transcript."},
                    {"inputSchema", {
                        {"type", "object"},
                        {"properties", {
                            {"run_id", {
                                {"type", "string"},
                                {"description", "Active run ID."}
                            }},
                            {"checkpoint_id", {
                                {"type", "string"},
                                {"description", "Checkpoint identifier (e.g. 'cp-01', 'step-5')."}
                            }},
                            {"commands", {
                                {"type", "array"},
                                {"items", {{"type", "string"}}},
                                {"description", "List of commands executed."}
                            }},
                            {"exit_codes", {
                                {"type", "array"},
                                {"items", {{"type", "integer"}}},
                                {"description", "List of command exit status codes."}
                            }},
                            {"dmesg_excerpt", {
                                {"type", "string"},
                                {"description", "Kernel logs, dmesg, or diagnostic snippet."}
                            }},
                            {"artifacts", {
                                {"type", "object"},
                                {"description", "Key-value map of hashes, output paths, or artifacts generated."}
                            }},
                            {"notes", {
                                {"type", "string"},
                                {"description", "Observations, analysis, or explanation of checkpoint execution."}
                            }}
                        }},
                        {"required", {"run_id", "checkpoint_id"}}
                    }}
                },
                {
                    {"name", "exec_review_checkpoint"},
                    {"description", "Records a reviewer decision on an executed checkpoint. Triggers human interlock if decision is wait_human."},
                    {"inputSchema", {
                        {"type", "object"},
                        {"properties", {
                            {"run_id", {
                                {"type", "string"},
                                {"description", "Active run ID."}
                            }},
                            {"seq", {
                                {"type", "integer"},
                                {"description", "Sequence number of the checkpoint result event being reviewed."}
                            }},
                            {"decision", {
                                {"type", "string"},
                                {"enum", {"continue", "stop", "recover", "wait_human"}},
                                {"description", "Review decision: 'continue', 'stop', 'recover', or 'wait_human'."}
                            }},
                            {"next_instructions", {
                                {"type", "string"},
                                {"description", "Instructions or command modifications for the executor next turn."}
                            }},
                            {"notes", {
                                {"type", "string"},
                                {"description", "Review findings or rationale for decision."}
                            }},
                            {"proposed_action", {
                                {"type", "string"},
                                {"description", "What action requires human approval if decision is 'wait_human'."}
                            }}
                        }},
                        {"required", {"run_id", "decision"}}
                    }}
                },
                {
                    {"name", "exec_interlock_wait"},
                    {"description", "Blocks until the human operator responds with Approve or Reject on the aimon dashboard."},
                    {"inputSchema", {
                        {"type", "object"},
                        {"properties", {
                            {"run_id", {
                                {"type", "string"},
                                {"description", "Run ID currently awaiting operator approval."}
                            }},
                            {"timeout_seconds", {
                                {"type", "integer"},
                                {"description", "Timeout in seconds (0 for indefinite blocking, default: 300)."}
                            }}
                        }},
                        {"required", {"run_id"}}
                    }}
                },
                {
                    {"name", "exec_get_transcript"},
                    {"description", "Fetches structured transcript events for a run, optionally starting after since_seq."},
                    {"inputSchema", {
                        {"type", "object"},
                        {"properties", {
                            {"run_id", {
                                {"type", "string"},
                                {"description", "Run ID to fetch transcript for (default: active run)."}
                            }},
                            {"since_seq", {
                                {"type", "integer"},
                                {"description", "Only fetch events with sequence number greater than this value (default: 0)."}
                            }}
                        }},
                        {"required", {"run_id"}}
                    }}
                }
            }}
        }}
    };

    if (_dynamicRegistry) {
        nlohmann::json dynTools = _dynamicRegistry->getToolsListJson();
        if (dynTools.is_array()) {
            for (const auto& dt : dynTools) {
                resp["result"]["tools"].push_back(dt);
            }
        }
    }

    return resp;
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
        task.agentName = args.value("agent_name", "");
        task.workspace = args.value("workspace", "");
        task.taskDescription = args.value("task_description", "");
        task.currentAction = args.value("current_action", "");
        task.status = args.value("status", "running");
        task.details = args.value("details", "");
        task.sseSessionId = args.value("sse_session_id", "");

        if (task.agentName.empty() || task.agentName == "Unknown Agent") {
            std::string verifiedName;
            if (!task.sseSessionId.empty() &&
                TaskRegistry::getInstance().getSessionClientName(task.sseSessionId, verifiedName)) {
                task.agentName = verifiedName;
            } else {
                task.agentName = "Unknown Agent";
            }
        }

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
    } else if (toolName == "agent_check_inbox") {
        nlohmann::json args = params.value("arguments", nlohmann::json::object());
        int timeoutSec = args.value("timeout_seconds", 0);
        std::string sessionId = args.value("sse_session_id", "");

        AgentMessage msg;
        bool gotMsg = AgentMessageBus::getInstance().fetchNextMessageForAgent(sessionId, timeoutSec, msg);
        if (gotMsg) {
            nlohmann::json res = {
                {"status", "message"},
                {"message_id", msg.messageId},
                {"session_id", msg.sessionId},
                {"text", msg.text},
                {"timestamp_epoch", msg.timestampEpoch}
            };
            contentText = res.dump(2);
        } else {
            nlohmann::json res = {
                {"status", "idle"},
                {"action", "poll_again"}
            };
            contentText = res.dump(2);
        }
    } else if (toolName == "agent_send_reply") {
        nlohmann::json args = params.value("arguments", nlohmann::json::object());
        std::string msgId = args.value("message_id", "");
        std::string replyText = args.value("reply_text", "");
        std::string sessionId = args.value("sse_session_id", "");
        if (sessionId.empty()) {
            sessionId = args.value("session_id", "");
        }

        if (sessionId.empty()) {
            auto sessions = TaskRegistry::getInstance().listSessions();
            if (!sessions.empty()) {
                sessionId = sessions.front().sessionId;
            }
        }

        bool delivered = AgentMessageBus::getInstance().postReplyFromAgent(sessionId, msgId, replyText);
        nlohmann::json res = {
            {"status", delivered ? "delivered" : "no_listener"},
            {"message_id", msgId}
        };
        contentText = res.dump(2);
    } else if (toolName == "agent_collaborate") {
        nlohmann::json args = params.value("arguments", nlohmann::json::object());
        std::string planFile = args.value("plan_file", "");
        int turnNumber = args.value("turn_number", 0);
        std::string agentId = args.value("agent_id", "");
        std::string agentModel = args.value("agent_model", "");
        std::string status = args.value("status", "IN_PROGRESS");
        std::string sideChannelMessage = args.value("side_channel_message", "");

        nlohmann::json outResult;
        std::string error;
        bool ok = AgentMessageBus::getInstance().signalCollaborationTurn(
            planFile, turnNumber, agentId, agentModel, status, sideChannelMessage, outResult, &error
        );

        if (!ok) {
            nlohmann::json errJson = {
                {"status", "error"},
                {"error", error}
            };
            contentText = errJson.dump(2);
        } else {
            contentText = outResult.dump(2);
        }
    } else if (toolName == "agent_wait_turn") {
        nlohmann::json args = params.value("arguments", nlohmann::json::object());
        std::string planFile = args.value("plan_file", "");
        std::string agentId = args.value("agent_id", "");
        int timeoutSeconds = args.value("timeout_seconds", 0);

        nlohmann::json outResult;
        std::string error;
        bool ok = AgentMessageBus::getInstance().waitCollaborationTurn(
            planFile, agentId, timeoutSeconds, outResult, &error
        );

        if (!ok) {
            nlohmann::json errJson = {
                {"status", "error"},
                {"error", error}
            };
            contentText = errJson.dump(2);
        } else {
            contentText = outResult.dump(2);
        }
    } else if (toolName == "exec_start_run") {
        nlohmann::json args = params.value("arguments", nlohmann::json::object());
        RunMetadata meta;
        meta.planFile = args.value("plan_file", "");
        meta.workspace = args.value("workspace", "");
        meta.targetAlias = args.value("target_alias", "");
        meta.executorId = args.value("executor_id", "agent-antigravity-builder");
        meta.initiatorId = args.value("initiator_id", "agent-cursor-windows");
        if (args.contains("run_id")) {
            meta.runId = args.value("run_id", "");
        }

        std::string runId;
        std::string err;
        bool ok = TranscriptSink::getInstance().startRun(meta, runId, err);
        if (!ok) {
            contentText = nlohmann::json({
                {"status", "error"},
                {"error", err}
            }).dump(2);
        } else {
            contentText = nlohmann::json({
                {"status", "started"},
                {"run_id", runId},
                {"target_alias", meta.targetAlias}
            }).dump(2);
        }
    } else if (toolName == "exec_submit_checkpoint") {
        nlohmann::json args = params.value("arguments", nlohmann::json::object());
        TranscriptEvent ev;
        ev.runId = args.value("run_id", "");
        ev.checkpointId = args.value("checkpoint_id", "");
        ev.actor = args.value("actor", "gemini");
        ev.kind = "result";
        if (args.contains("commands") && args["commands"].is_array()) {
            ev.commands = args["commands"];
        }
        if (args.contains("exit_codes") && args["exit_codes"].is_array()) {
            ev.exitCodes = args["exit_codes"];
        }
        if (args.contains("artifacts") && args["artifacts"].is_object()) {
            ev.artifacts = args["artifacts"];
        }
        ev.dmesgExcerpt = args.value("dmesg_excerpt", "");
        ev.notes = args.value("notes", "");

        std::string err;
        int seq = TranscriptSink::getInstance().appendEvent(ev, err);
        if (seq < 0) {
            contentText = nlohmann::json({
                {"status", "error"},
                {"error", err}
            }).dump(2);
        } else {
            contentText = nlohmann::json({
                {"status", "ok"},
                {"run_id", ev.runId},
                {"checkpoint_id", ev.checkpointId},
                {"seq", seq}
            }).dump(2);
        }
    } else if (toolName == "exec_review_checkpoint") {
        nlohmann::json args = params.value("arguments", nlohmann::json::object());
        TranscriptEvent ev;
        ev.runId = args.value("run_id", "");
        ev.seq = args.value("seq", 0);
        ev.actor = args.value("actor", "cursor");
        ev.kind = "review";
        ev.decision = args.value("decision", "continue");
        ev.proposalNext = args.value("next_instructions", "");
        ev.notes = args.value("notes", "");

        std::string err;
        int seq = TranscriptSink::getInstance().appendEvent(ev, err);
        if (seq < 0) {
            contentText = nlohmann::json({
                {"status", "error"},
                {"error", err}
            }).dump(2);
        } else {
            std::string interlockId;
            if (ev.decision == "wait_human") {
                std::string proposed = args.value("proposed_action", ev.proposalNext);
                interlockId = InterlockManager::getInstance().createInterlock(
                    ev.runId, ev.checkpointId, seq, ev.notes, proposed
                );
            }
            nlohmann::json res = {
                {"status", "ok"},
                {"run_id", ev.runId},
                {"seq", seq},
                {"decision", ev.decision}
            };
            if (!interlockId.empty()) {
                res["interlock_id"] = interlockId;
            }
            contentText = res.dump(2);
        }
    } else if (toolName == "exec_interlock_wait") {
        nlohmann::json args = params.value("arguments", nlohmann::json::object());
        std::string runId = args.value("run_id", "");
        std::string interlockId = args.value("interlock_id", "");
        int timeoutSec = args.value("timeout_seconds", 300);

        std::string target = !interlockId.empty() ? interlockId : runId;
        InterlockRequest result;
        bool ok = InterlockManager::getInstance().waitInterlock(target, timeoutSec, result);
        if (!ok) {
            contentText = nlohmann::json({
                {"status", "timeout"},
                {"approved", false},
                {"reject_reason", "Timeout waiting for operator response"}
            }).dump(2);
        } else {
            contentText = nlohmann::json({
                {"status", "resolved"},
                {"interlock_id", result.interlockId},
                {"run_id", result.runId},
                {"approved", result.approved},
                {"reject_reason", result.rejectReason}
            }).dump(2);
        }
    } else if (toolName == "exec_get_transcript") {
        nlohmann::json args = params.value("arguments", nlohmann::json::object());
        std::string runId = args.value("run_id", "");
        int sinceSeq = args.value("since_seq", 0);

        auto events = TranscriptSink::getInstance().getTranscript(runId, sinceSeq);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& ev : events) {
            arr.push_back(ev.toJson());
        }
        nlohmann::json res = {
            {"run_id", runId.empty() ? TranscriptSink::getInstance().getActiveRunId() : runId},
            {"event_count", (int)arr.size()},
            {"events", arr}
        };
        contentText = res.dump(2);
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
                    {"code", -32000},
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
