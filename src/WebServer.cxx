/*
 * WebServer.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "WebServer.hxx"
#include "WebAssets.hxx"
#include "McpServer.hxx"
#include "TaskRegistry.hxx"
#include "AgentMessageBus.hxx"
#include "CollabOrchestrator.hxx"
#include "TranscriptSink.hxx"
#include "InterlockManager.hxx"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <random>
#include <sstream>
#include <iomanip>
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include <httplib.h>

namespace fs = std::filesystem;

namespace aimon {

static std::string generateSessionId() {
    static thread_local std::random_device rd;
    static thread_local std::mt19937_64 gen(rd());
    static thread_local std::uniform_int_distribution<uint64_t> dis;
    uint64_t part1 = dis(gen);
    uint64_t part2 = dis(gen);
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << part1 << std::setw(16) << part2;
    return oss.str();
}

WebServer::WebServer(StateStore& stateStore, HistoryStore& historyStore,
                     const WebConfig& config, RefreshCallback onRefresh,
                     McpServer* mcpServer)
    : _stateStore(stateStore),
      _historyStore(historyStore),
      _config(config),
      _onRefresh(onRefresh),
      _mcpServer(mcpServer),
      _server(std::make_unique<httplib::Server>()) {
}

WebServer::~WebServer() {
    stop();
}

void WebServer::setupRoutes() {
    AgentMessageBus::getInstance().setCollaborationCallback([this](const CollaborationSession& session, const std::string& eventType) {
        nlohmann::json evt = {
            {"event", eventType},
            {"session", session.toJson()}
        };
        std::string sseData = evt.dump();
        std::lock_guard<std::mutex> lock(_sessionsMutex);
        for (auto& pair : _sseSessions) {
            auto s = pair.second;
            if (s && !s->closed.load()) {
                std::lock_guard<std::mutex> slock(s->mutex);
                s->messageQueue.push(sseData);
                s->cv.notify_one();
            }
        }
    });

    InterlockManager::getInstance().setNotifyCallback([this](const InterlockRequest& req, const std::string& eventType) {
        nlohmann::json evt = {
            {"event", eventType},
            {"interlock", req.toJson()}
        };
        std::string sseData = evt.dump();
        std::lock_guard<std::mutex> lock(_sessionsMutex);
        for (auto& pair : _sseSessions) {
            auto s = pair.second;
            if (s && !s->closed.load()) {
                std::lock_guard<std::mutex> slock(s->mutex);
                s->messageQueue.push(sseData);
                s->cv.notify_one();
            }
        }
    });

    auto serveFileOrFallback = [](const std::string& diskPath,
                                  const char* fallbackAsset,
                                  const std::string& contentType,
                                  httplib::Response& res) {
        if (fs::exists(diskPath)) {
            std::ifstream f(diskPath);
            if (f.is_open()) {
                std::string content((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
                res.set_content(content, contentType.c_str());
                return;
            }
        }
        res.set_content(fallbackAsset, contentType.c_str());
    };

    _server->Get("/", [serveFileOrFallback](const httplib::Request&, httplib::Response& res) {
        serveFileOrFallback("web/index.html", assets::INDEX_HTML, "text/html", res);
    });

    _server->Get("/style.css", [serveFileOrFallback](const httplib::Request&, httplib::Response& res) {
        serveFileOrFallback("web/style.css", assets::STYLE_CSS, "text/css", res);
    });

    _server->Get("/app.js", [serveFileOrFallback](const httplib::Request&, httplib::Response& res) {
        serveFileOrFallback("web/app.js", assets::APP_JS, "application/javascript", res);
    });

    _server->Get("/api/status", [this](const httplib::Request&, httplib::Response& res) {
        std::string jsonStr = _stateStore.getStatus().toJson().dump(2);
        res.set_content(jsonStr, "application/json");
        res.set_header("Access-Control-Allow-Origin", "*");
    });

    _server->Post("/api/refresh", [this](const httplib::Request&, httplib::Response& res) {
        if (_onRefresh) {
            try {
                _onRefresh();
            } catch (...) {
            }
        }
        std::string jsonStr = _stateStore.getStatus().toJson().dump(2);
        res.set_content(jsonStr, "application/json");
        res.set_header("Access-Control-Allow-Origin", "*");
    });

    _server->Get("/api/history", [this](const httplib::Request& req, httplib::Response& res) {
        int limit = 50;
        if (req.has_param("limit")) {
            try {
                limit = std::stoi(req.get_param_value("limit"));
            } catch (...) {}
        }
        auto records = _historyStore.queryRecentRecords(limit);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& r : records) {
            arr.push_back({
                {"id", r.id},
                {"timestamp", r.timestamp},
                {"provider", r.provider},
                {"metric_key", r.metricKey},
                {"metric_value", r.metricValue},
                {"metric_limit", r.metricLimit},
                {"delta", r.delta}
            });
        }
        res.set_content(arr.dump(2), "application/json");
        res.set_header("Access-Control-Allow-Origin", "*");
    });

    _server->Get("/api/tasks", [this](const httplib::Request& req, httplib::Response& res) {
        bool includeCompleted = false;
        if (req.has_param("include_completed")) {
            std::string val = req.get_param_value("include_completed");
            includeCompleted = (val == "true" || val == "1");
        }
        auto tasks = TaskRegistry::getInstance().listTasks(includeCompleted);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& t : tasks) {
            arr.push_back(t.toJson());
        }
        res.set_content(arr.dump(2), "application/json");
        res.set_header("Access-Control-Allow-Origin", "*");
    });

    _server->Post("/api/tasks/register", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        try {
            auto body = nlohmann::json::parse(req.body);
            AgentTask task;
            task.taskId = body.value("task_id", "");
            task.agentName = body.value("agent_name", "Unknown Agent");
            task.workspace = body.value("workspace", "");
            task.taskDescription = body.value("task_description", "");
            task.currentAction = body.value("current_action", "");
            task.status = body.value("status", "running");
            task.details = body.value("details", "");

            std::string registeredId = TaskRegistry::getInstance().registerOrUpdateTask(task);
            AgentTask savedTask;
            TaskRegistry::getInstance().getTask(registeredId, savedTask);

            nlohmann::json resp = {
                {"status", "ok"},
                {"task", savedTask.toJson()}
            };
            res.set_content(resp.dump(2), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            nlohmann::json err = {{"error", e.what()}};
            res.set_content(err.dump(2), "application/json");
        }
    });

    _server->Post("/api/tasks/complete", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        try {
            auto body = nlohmann::json::parse(req.body);
            std::string taskId = body.value("task_id", "");
            std::string status = body.value("status", "completed");
            std::string details = body.value("details", "");

            bool ok = TaskRegistry::getInstance().completeTask(taskId, status, details);
            if (ok) {
                AgentTask task;
                TaskRegistry::getInstance().getTask(taskId, task);
                res.set_content(task.toJson().dump(2), "application/json");
            } else {
                res.status = 404;
                res.set_content(nlohmann::json({{"error", "Task not found"}}).dump(), "application/json");
            }
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(nlohmann::json({{"error", e.what()}}).dump(), "application/json");
        }
    });

    _server->Post("/api/tasks/clear", [this](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        TaskRegistry::getInstance().clear();
        res.set_content(nlohmann::json({{"status", "ok"}, {"cleared", true}}).dump(2), "application/json");
    });

    _server->Get("/api/sessions", [this](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        auto sessions = TaskRegistry::getInstance().listSessions();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& s : sessions) {
            arr.push_back(s.toJson());
        }
        res.set_content(arr.dump(2), "application/json");
    });

    _server->Get("/api/collaboration/status", [this](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        auto session = AgentMessageBus::getInstance().getActiveCollaboration();
        nlohmann::json j = session.toJson();
        if (_collabOrch) {
            j["orchestrator"] = _collabOrch->statusJson();
        }
        res.set_content(j.dump(2), "application/json");
    });

    _server->Post("/api/collaboration/auto", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        if (!_collabOrch) {
            res.status = 503;
            res.set_content(nlohmann::json({{"error", "Orchestrator not initialized"}}).dump(), "application/json");
            return;
        }
        try {
            auto body = nlohmann::json::parse(req.body.empty() ? "{}" : req.body);
            bool enable = body.value("enabled", true);
            _collabOrch->setAutoDrive(enable);
            res.set_content(nlohmann::json({{"status", "ok"}, {"auto_drive", enable}}).dump(2), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(nlohmann::json({{"error", e.what()}}).dump(), "application/json");
        }
    });

    _server->Post("/api/collaboration/run", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        if (!_collabOrch) {
            res.status = 503;
            res.set_content(nlohmann::json({{"error", "Orchestrator not initialized"}}).dump(), "application/json");
            return;
        }
        try {
            auto body = nlohmann::json::parse(req.body.empty() ? "{}" : req.body);
            std::string planFile = body.value("plan_file", "");
            std::string initId = body.value("initiator_id", "agent-antigravity-builder");
            std::string revId = body.value("reviewer_id", "agent-cursor-windows");
            std::string err;
            if (!_collabOrch->runUntilConsensus(planFile, initId, revId, &err)) {
                res.status = 400;
                res.set_content(nlohmann::json({{"error", err}}).dump(), "application/json");
            } else {
                if (body.contains("max_turns") && body["max_turns"].is_number()) {
                    AgentMessageBus::getInstance().setMaxTurns(body["max_turns"].get<int>());
                }
                res.set_content(nlohmann::json({{"status", "ok"}, {"message", "Orchestrator driving session"}}).dump(2), "application/json");
            }
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(nlohmann::json({{"error", e.what()}}).dump(), "application/json");
        }
    });

    _server->Post("/api/collaboration/step", [this](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        if (!_collabOrch) {
            res.status = 503;
            res.set_content(nlohmann::json({{"error", "Orchestrator not initialized"}}).dump(), "application/json");
            return;
        }
        std::string err;
        if (!_collabOrch->stepTurn(&err)) {
            res.status = 400;
            res.set_content(nlohmann::json({{"error", err}}).dump(), "application/json");
        } else {
            res.set_content(nlohmann::json({{"status", "ok"}, {"message", "Step requested"}}).dump(2), "application/json");
        }
    });

    _server->Post("/api/collaboration/start", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        try {
            auto body = nlohmann::json::parse(req.body.empty() ? "{}" : req.body);
            std::string planFile = body.value("plan_file", "");
            std::string initiatorId = body.value("initiator_id", "agent-antigravity-builder");
            std::string reviewerId = body.value("reviewer_id", "agent-cursor-windows");
            std::string err;
            bool ok = AgentMessageBus::getInstance().startCollaboration(planFile, initiatorId, reviewerId, &err);
            if (!ok) {
                res.status = 409;
                nlohmann::json errObj;
                errObj["error"] = err;
                res.set_content(errObj.dump(2), "application/json");
            } else {
                if (body.contains("max_turns") && body["max_turns"].is_number()) {
                    AgentMessageBus::getInstance().setMaxTurns(body["max_turns"].get<int>());
                }
                auto session = AgentMessageBus::getInstance().getActiveCollaboration();
                nlohmann::json resp;
                resp["status"] = "ok";
                resp["session"] = session.toJson();
                res.set_content(resp.dump(2), "application/json");
            }
        } catch (const std::exception& e) {
            res.status = 400;
            nlohmann::json errObj;
            errObj["error"] = e.what();
            res.set_content(errObj.dump(2), "application/json");
        }
    });

    _server->Post("/api/collaboration/collaborate", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        try {
            auto body = nlohmann::json::parse(req.body.empty() ? "{}" : req.body);
            std::string planFile = body.value("plan_file", "");
            int turnNumber = body.value("turn_number", 0);
            std::string agentId = body.value("agent_id", "");
            std::string agentModel = body.value("agent_model", "");
            std::string status = body.value("status", "IN_PROGRESS");
            std::string sideChannelMessage = body.value("side_channel_message", "");

            nlohmann::json outResult;
            std::string err;
            bool ok = AgentMessageBus::getInstance().signalCollaborationTurn(
                planFile, turnNumber, agentId, agentModel, status, sideChannelMessage, outResult, &err
            );
            if (!ok) {
                res.status = 400;
                nlohmann::json errObj;
                errObj["error"] = err;
                res.set_content(errObj.dump(2), "application/json");
            } else {
                res.set_content(outResult.dump(2), "application/json");
            }
        } catch (const std::exception& e) {
            res.status = 400;
            nlohmann::json errObj;
            errObj["error"] = e.what();
            res.set_content(errObj.dump(2), "application/json");
        }
    });

    _server->Post("/api/collaboration/nudge", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        std::string err;
        bool ok = AgentMessageBus::getInstance().nudgeCollaboration(&err);
        if (!ok) {
            res.status = 400;
            nlohmann::json errObj;
            errObj["error"] = err;
            res.set_content(errObj.dump(2), "application/json");
        } else {
            nlohmann::json resp;
            resp["status"] = "nudged";
            res.set_content(resp.dump(2), "application/json");
        }
    });

    _server->Post("/api/collaboration/takeover", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        try {
            auto body = nlohmann::json::parse(req.body.empty() ? "{}" : req.body);
            std::string agentId = body.value("agent_id", "operator");
            std::string err;
            bool ok = AgentMessageBus::getInstance().takeoverCollaboration(agentId, &err);
            if (!ok) {
                res.status = 400;
                nlohmann::json errObj;
                errObj["error"] = err;
                res.set_content(errObj.dump(2), "application/json");
            } else {
                nlohmann::json resp;
                resp["status"] = "taken_over";
                resp["next_actor_id"] = agentId;
                res.set_content(resp.dump(2), "application/json");
            }
        } catch (const std::exception& e) {
            res.status = 400;
            nlohmann::json errObj;
            errObj["error"] = e.what();
            res.set_content(errObj.dump(2), "application/json");
        }
    });

    _server->Post("/api/collaboration/abort", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        std::string err;
        bool ok = AgentMessageBus::getInstance().abortCollaboration(&err);
        if (!ok) {
            res.status = 400;
            nlohmann::json errObj;
            errObj["error"] = err;
            res.set_content(errObj.dump(2), "application/json");
        } else {
            nlohmann::json resp;
            resp["status"] = "aborted";
            res.set_content(resp.dump(2), "application/json");
        }
    });

    // --- Execution Runs & Transcript Endpoints ---
    _server->Get("/api/exec/runs", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        auto runs = TranscriptSink::getInstance().listRuns();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& r : runs) {
            arr.push_back(r.toJson());
        }
        nlohmann::json resp = {
            {"active_run_id", TranscriptSink::getInstance().getActiveRunId()},
            {"runs", arr}
        };
        res.set_content(resp.dump(2), "application/json");
    });

    _server->Post(R"(/api/exec/runs/([^/]+)/end)", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        std::string runId = req.matches[1].str();
        std::string status = "completed";
        if (!req.body.empty()) {
            try {
                auto body = nlohmann::json::parse(req.body);
                status = body.value("status", "completed");
            } catch (...) {}
        }
        std::string err;
        bool ok = TranscriptSink::getInstance().endRun(runId, status, err);
        if (!ok) {
            res.status = 400;
            res.set_content(nlohmann::json({{"error", err}}).dump(2), "application/json");
        } else {
            res.set_content(nlohmann::json({
                {"status", "ok"},
                {"run_id", runId},
                {"terminal_status", status}
            }).dump(2), "application/json");
        }
    });

    _server->Get(R"(/api/exec/runs/([^/]+)/transcript)", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        std::string runId = req.matches[1].str();
        int sinceSeq = 0;
        if (req.has_param("since_seq")) {
            try {
                sinceSeq = std::stoi(req.get_param_value("since_seq"));
            } catch (...) {}
        }
        auto events = TranscriptSink::getInstance().getTranscript(runId, sinceSeq);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& ev : events) {
            arr.push_back(ev.toJson());
        }
        RunMetadata meta;
        bool hasMeta = TranscriptSink::getInstance().getRunMetadata(runId, meta);
        nlohmann::json resp = {
            {"run_id", runId},
            {"metadata", hasMeta ? meta.toJson() : nlohmann::json::object()},
            {"event_count", (int)arr.size()},
            {"events", arr}
        };
        res.set_content(resp.dump(2), "application/json");
    });

    _server->Get("/api/exec/interlocks", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        std::string runId = req.has_param("run_id") ? req.get_param_value("run_id") : "";
        bool includeResolved = req.has_param("include_resolved") &&
            (req.get_param_value("include_resolved") == "true" || req.get_param_value("include_resolved") == "1");

        auto list = includeResolved ?
            InterlockManager::getInstance().listInterlocks(runId) :
            InterlockManager::getInstance().getPendingInterlocks(runId);

        nlohmann::json arr = nlohmann::json::array();
        for (const auto& item : list) {
            arr.push_back(item.toJson());
        }
        res.set_content(arr.dump(2), "application/json");
    });

    _server->Post(R"(/api/exec/interlocks/([^/]+)/resolve)", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        std::string interlockId = req.matches[1].str();
        try {
            auto body = nlohmann::json::parse(req.body.empty() ? "{}" : req.body);
            bool approved = body.value("approved", false);
            std::string reason = body.value("reason", "");
            bool ok = InterlockManager::getInstance().resolveInterlock(interlockId, approved, reason);
            if (!ok) {
                res.status = 404;
                res.set_content(nlohmann::json({
                    {"error", "Interlock not found or already resolved: " + interlockId}
                }).dump(2), "application/json");
            } else {
                res.set_content(nlohmann::json({
                    {"status", "ok"},
                    {"interlock_id", interlockId},
                    {"approved", approved},
                    {"reason", reason}
                }).dump(2), "application/json");
            }
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(nlohmann::json({{"error", e.what()}}).dump(2), "application/json");
        }
    });

    _server->Get("/sse", [this](const httplib::Request& req, httplib::Response& res) {
        if (!_mcpServer) {
            res.status = 503;
            res.set_content("MCP service not configured", "text/plain");
            return;
        }

        std::string sessionId = generateSessionId();
        auto session = std::make_shared<SseSession>();
        session->id = sessionId;

        {
            std::lock_guard<std::mutex> lock(_sessionsMutex);
            _sseSessions[sessionId] = session;
        }

        std::string remoteIp = req.remote_addr;
        TaskRegistry::getInstance().registerSession(sessionId, remoteIp);

        std::cout << "[WebServer] New SSE client connected from " << remoteIp
                  << ", session: " << sessionId << std::endl;

        res.set_header("Content-Type", "text/event-stream");
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection", "keep-alive");
        res.set_header("Access-Control-Allow-Origin", "*");

        auto provider = [session, initialSent = false](size_t offset, httplib::DataSink& sink) mutable -> bool {
            (void)offset;
            if (!sink.is_writable() || session->closed.load()) {
                return false;
            }

            if (!initialSent) {
                std::string endpointMsg = "event: endpoint\ndata: /message?sessionId=" + session->id + "\n\n";
                initialSent = true;
                return sink.write(endpointMsg.data(), endpointMsg.size());
            }

            std::unique_lock<std::mutex> lock(session->mutex);
            session->cv.wait_for(lock, std::chrono::seconds(15), [&]() {
                return !session->messageQueue.empty() || session->closed.load();
            });

            if (session->closed.load()) {
                return false;
            }

            if (session->messageQueue.empty()) {
                std::string ping = ": keepalive\n\n";
                return sink.write(ping.data(), ping.size());
            }

            std::string msg = session->messageQueue.front();
            session->messageQueue.pop();
            lock.unlock();

            std::string sseData = "event: message\ndata: " + msg + "\n\n";
            return sink.write(sseData.data(), sseData.size());
        };

        auto releaser = [this, sessionId, session](bool success) {
            (void)success;
            session->closed.store(true);
            session->cv.notify_all();
            std::lock_guard<std::mutex> lock(_sessionsMutex);
            _sseSessions.erase(sessionId);
            TaskRegistry::getInstance().handleSessionDisconnected(sessionId);
            TaskRegistry::getInstance().removeSession(sessionId);
            std::cout << "[WebServer] SSE client disconnected, session: " << sessionId << std::endl;
        };

        res.set_chunked_content_provider("text/event-stream", provider, releaser);
    });

    auto handleMcpMessage = [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization, Accept, Mcp-Session-Id, mcp-session-id");
        if (!_mcpServer) {
            res.status = 503;
            res.set_content("MCP service not configured", "text/plain");
            return;
        }

        // Extract session ID from query parameters or headers
        std::string sessionId;
        if (req.has_param("sessionId")) {
            sessionId = req.get_param_value("sessionId");
        } else if (req.has_param("session_id")) {
            sessionId = req.get_param_value("session_id");
        } else if (req.has_header("Mcp-Session-Id")) {
            sessionId = req.get_header_value("Mcp-Session-Id");
        } else if (req.has_header("mcp-session-id")) {
            sessionId = req.get_header_value("mcp-session-id");
        }

        // Strip trailing carriage return, newline, or whitespace
        while (!sessionId.empty() && (sessionId.back() == '\r' || sessionId.back() == '\n' || sessionId.back() == ' ')) {
            sessionId.pop_back();
        }

        nlohmann::json reqJson;
        try {
            reqJson = nlohmann::json::parse(req.body);
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(std::string("Invalid JSON: ") + e.what(), "text/plain");
            return;
        }

        std::string method = reqJson.value("method", "");
        std::cout << "[WebServer] MCP POST: method=" << method << " session='" << sessionId << "'" << std::endl;

        if (!sessionId.empty()) {
            TaskRegistry::getInstance().touchSession(sessionId);
        }

        if (method == "initialize" && !sessionId.empty()) {
            if (reqJson.contains("params") && reqJson["params"].is_object()) {
                const auto& p = reqJson["params"];
                if (p.contains("clientInfo") && p["clientInfo"].is_object()) {
                    const auto& ci = p["clientInfo"];
                    std::string cName = ci.value("name", "MCP Client");
                    std::string cVer = ci.value("version", "");
                    TaskRegistry::getInstance().updateSessionClientInfo(sessionId, cName, cVer);
                    std::cout << "[WebServer] MCP client identified for session " << sessionId
                              << ": name='" << cName << "', version='" << cVer << "'" << std::endl;
                }
            }
        }

        if (method == "tools/call" && !sessionId.empty()) {
            if (reqJson.contains("params") && reqJson["params"].is_object()) {
                auto& p = reqJson["params"];
                std::string tName = p.value("name", "");
                if (tName == "register_agent_task" ||
                    tName == "agent_check_inbox" ||
                    tName == "agent_send_reply") {
                    if (!p.contains("arguments") || !p["arguments"].is_object()) {
                        p["arguments"] = nlohmann::json::object();
                    }
                    p["arguments"]["sse_session_id"] = sessionId;
                }
            }
        }

        // Process message through MCP server engine
        nlohmann::json respJson = _mcpServer->handleMessage(reqJson);
        std::string respStr = respJson.is_null() ? "{}" : respJson.dump();

        // Check if an SSE session exists for push notification
        std::shared_ptr<SseSession> session;
        if (!sessionId.empty()) {
            std::lock_guard<std::mutex> lock(_sessionsMutex);
            auto it = _sseSessions.find(sessionId);
            if (it != _sseSessions.end()) {
                session = it->second;
            }
        }

        if (session && !respJson.is_null()) {
            {
                std::lock_guard<std::mutex> lock(session->mutex);
                session->messageQueue.push(respStr);
            }
            session->cv.notify_one();
        }

        // Return JSON response directly in HTTP response body (supports Streamable HTTP)
        if (!sessionId.empty()) {
            res.set_header("Mcp-Session-Id", sessionId);
        }
        res.status = 200;
        res.set_content(respStr, "application/json");
    };

    _server->Post("/message", handleMcpMessage);
    _server->Post("/messages", handleMcpMessage);
    _server->Post("/sse", handleMcpMessage);

    auto handleOptions = [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization, Accept, Mcp-Session-Id, mcp-session-id");
        res.status = 204;
    };
    _server->Options("/sse", handleOptions);
    _server->Options("/message", handleOptions);
    _server->Options("/messages", handleOptions);
}

bool WebServer::start(bool async) {
    if (_running) return true;

    setupRoutes();
    _running = true;

    std::cout << "[WebServer] Dashboard available at http://"
              << _config.host << ":" << _config.port << std::endl;
    if (_mcpServer) {
        std::cout << "[WebServer] MCP SSE endpoint available at http://"
                  << _config.host << ":" << _config.port << "/sse" << std::endl;
    }

    if (async) {
        _thread = std::make_unique<std::thread>([this]() {
            if (!_server->listen(_config.host.c_str(), _config.port)) {
                std::cerr << "[WebServer] Failed to bind to "
                          << _config.host << ":" << _config.port << std::endl;
                _running = false;
            }
        });
        // Short pause to allow server to bind
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return _running;
    } else {
        return _server->listen(_config.host.c_str(), _config.port);
    }
}

void WebServer::broadcastSseNotification(const std::string& jsonRpcNotification) {
    std::lock_guard<std::mutex> lock(_sessionsMutex);
    for (auto& pair : _sseSessions) {
        auto& session = pair.second;
        if (session && !session->closed.load()) {
            {
                std::lock_guard<std::mutex> sLock(session->mutex);
                session->messageQueue.push(jsonRpcNotification);
            }
            session->cv.notify_one();
        }
    }
}

void WebServer::stop() {
    if (!_running) return;

    _running = false;
    {
        std::lock_guard<std::mutex> lock(_sessionsMutex);
        for (auto& pair : _sseSessions) {
            pair.second->closed.store(true);
            pair.second->cv.notify_all();
        }
        _sseSessions.clear();
    }

    if (_server) {
        _server->stop();
    }
    if (_thread && _thread->joinable()) {
        _thread->join();
        _thread.reset();
    }
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
