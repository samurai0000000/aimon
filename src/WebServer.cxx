/*
 * WebServer.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "WebServer.hxx"
#include "WebAssets.hxx"
#include "McpServer.hxx"
#include "TaskRegistry.hxx"
#include "TcpGateway.hxx"
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
                     McpServer* mcpServer,
                     TcpGateway* tcpGateway)
    : _stateStore(stateStore),
      _historyStore(historyStore),
      _config(config),
      _onRefresh(onRefresh),
      _mcpServer(mcpServer),
      _tcpGateway(tcpGateway),
      _endpointsEnabled(config.endpointsEnabled),
      _server(std::make_unique<httplib::Server>()) {
}

WebServer::~WebServer() {
    stop();
}

std::string WebServer::createUiSession() {
    static thread_local std::random_device rd;
    static thread_local std::mt19937_64 gen(rd());
    static thread_local std::uniform_int_distribution<uint64_t> dis;

    uint64_t p1 = dis(gen);
    uint64_t p2 = dis(gen);
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << p1 << std::setw(16) << p2;
    std::string token = oss.str();

    std::lock_guard<std::mutex> lock(_sessionMutex);
    time_t now = time(nullptr);
    for (auto it = _uiSessions.begin(); it != _uiSessions.end(); ) {
        if (now - it->second > 86400 * 7) {
            it = _uiSessions.erase(it);
        } else {
            ++it;
        }
    }
    _uiSessions[token] = now;
    return token;
}

bool WebServer::isValidUiSession(const httplib::Request& req) const {
    // 1. Block command-line agents and automated scripts
    if (req.has_header("User-Agent")) {
        std::string ua = req.get_header_value("User-Agent");
        for (char &c : ua) c = tolower(c);
        if (ua.find("curl/") != std::string::npos ||
            ua.find("python") != std::string::npos ||
            ua.find("wget/") != std::string::npos ||
            ua.find("httpie") != std::string::npos ||
            ua.find("aiohttp") != std::string::npos ||
            ua.find("go-http-client") != std::string::npos) {
            return false;
        }
    } else {
        return false;
    }

    // 2. Extract session token
    std::string token;
    if (req.has_header("X-UI-Session")) {
        token = req.get_header_value("X-UI-Session");
    } else if (req.has_header("Cookie")) {
        std::string cookie = req.get_header_value("Cookie");
        size_t pos = cookie.find("aimon_session=");
        if (pos != std::string::npos) {
            size_t start = pos + 14;
            size_t end = cookie.find(';', start);
            token = (end == std::string::npos) ? cookie.substr(start) : cookie.substr(start, end - start);
        }
    }

    if (token.empty()) {
        return false;
    }

    // 3. Verify token exists and is fresh
    std::lock_guard<std::mutex> lock(_sessionMutex);
    auto it = _uiSessions.find(token);
    if (it != _uiSessions.end()) {
        time_t now = time(nullptr);
        if (now - it->second < 86400 * 7) {
            return true;
        }
    }

    return false;
}

std::string WebServer::getMcpHintForPath(const std::string& path, const std::string& body) {
    (void)body;
    if (path == "/api/status" || path == "/api/history") {
        return "Use official MCP tool 'get_combined_ai_status', 'check_antigravity_quota', or 'check_cursor_usage'.";
    }
    return "Use official MCP tools ('aimon' gateway or native tools). Direct REST API access is disabled.";
}

void WebServer::setupRoutes() {
    // Gate REST API endpoints if disabled by configuration, unless request is from an authenticated Web UI session
    if (!_endpointsEnabled) {
        _server->set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
            if (req.path == "/api/monitors") {
                return httplib::Server::HandlerResponse::Unhandled;
            }
            if (req.path == "/api" || req.path.rfind("/api/", 0) == 0) {
                if (!isValidUiSession(req)) {
                    res.status = 403;
                    std::string hint = getMcpHintForPath(req.path, req.body);
                    nlohmann::json err = {
                        {"error", "Direct REST API endpoint access is disabled by configuration."},
                        {"hint", hint},
                        {"daemon", "aimon"}
                    };
                    res.set_content(err.dump(2), "application/json");
                    return httplib::Server::HandlerResponse::Handled;
                }
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });
    }

    auto serveFileOrFallback = [](const std::string& diskPath,
                                  const char* fallbackAsset,
                                  const std::string& contentType,
                                  httplib::Response& res) {
        res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
        res.set_header("Pragma", "no-cache");
        res.set_header("Expires", "0");
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

    _server->Get("/", [this, serveFileOrFallback](const httplib::Request&, httplib::Response& res) {
        std::string token = createUiSession();
        res.set_header("Set-Cookie", "aimon_session=" + token + "; Path=/; SameSite=Strict");
        res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
        res.set_header("Pragma", "no-cache");
        res.set_header("Expires", "0");

        std::string html;
        if (fs::exists("web/index.html")) {
            std::ifstream f("web/index.html");
            if (f.is_open()) {
                html = std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            }
        }
        if (html.empty()) {
            html = assets::INDEX_HTML;
        }

        std::string sessionScript = "<script>\n"
            "window.__UI_SESSION_TOKEN__ = \"" + token + "\";\n"
            "(function() {\n"
            "    const originalFetch = window.fetch;\n"
            "    window.fetch = function(url, options) {\n"
            "        options = options || {};\n"
            "        options.headers = options.headers || {};\n"
            "        if (window.__UI_SESSION_TOKEN__) {\n"
            "            if (options.headers instanceof Headers) {\n"
            "                options.headers.set('X-UI-Session', window.__UI_SESSION_TOKEN__);\n"
            "            } else if (Array.isArray(options.headers)) {\n"
            "                options.headers.push(['X-UI-Session', window.__UI_SESSION_TOKEN__]);\n"
            "            } else {\n"
            "                options.headers['X-UI-Session'] = window.__UI_SESSION_TOKEN__;\n"
            "            }\n"
            "        }\n"
            "        return originalFetch(url, options);\n"
            "    };\n"
            "})();\n"
            "</script>\n";

        size_t headPos = html.find("</head>");
        if (headPos != std::string::npos) {
            html.insert(headPos, sessionScript);
        } else {
            html = sessionScript + html;
        }

        res.set_content(html, "text/html");
    });

    _server->Get("/style.css", [serveFileOrFallback](const httplib::Request&, httplib::Response& res) {
        serveFileOrFallback("web/style.css", assets::STYLE_CSS, "text/css", res);
    });

    _server->Get("/app.js", [serveFileOrFallback](const httplib::Request&, httplib::Response& res) {
        serveFileOrFallback("web/app.js", assets::APP_JS, "application/javascript", res);
    });

    _server->Get("/api/status", [this](const httplib::Request&, httplib::Response& res) {
        std::string jsonStr = _stateStore.getStatus().toJson().dump(2);
        res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
        res.set_header("Pragma", "no-cache");
        res.set_header("Expires", "0");
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
        res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
        res.set_header("Pragma", "no-cache");
        res.set_header("Expires", "0");
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

    _server->Get("/api/sessions", [this](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        auto sessions = TaskRegistry::getInstance().listSessions();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& s : sessions) {
            arr.push_back(s.toJson());
        }
        res.set_content(arr.dump(2), "application/json");
    });

    _server->Get("/api/monitors", [this](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
        res.set_header("Pragma", "no-cache");
        res.set_header("Expires", "0");
        nlohmann::json arr = nlohmann::json::array();
        if (_tcpGateway) {
            auto monitors = _tcpGateway->getDiscoveredMonitors(_config.port);
            for (const auto& m : monitors) {
                arr.push_back(m.toJson());
            }
        } else {
            DiscoveredMonitor selfMon;
            selfMon.id = "aimon";
            selfMon.name = "AI Quotas";
            selfMon.shortName = "AI Quotas";
            selfMon.subsystem = "aimon";
            selfMon.host = "127.0.0.1";
            selfMon.port = _config.port;
            selfMon.path = "/";
            selfMon.connected = true;
            selfMon.reachable = true;
            selfMon.isSelf = true;
            selfMon.priority = 0;
            selfMon.lastSeenEpoch = std::time(nullptr);
            arr.push_back(selfMon.toJson());
        }
        res.set_content(arr.dump(2), "application/json");
    });

    _server->Get("/sse", [this](const httplib::Request& req, httplib::Response& res) {
        if (!_mcpServer) {
            res.status = 503;
            res.set_content("MCP service not configured", "text/plain");
            return;
        }

        std::string profile;
        if (req.has_param("profile")) {
            profile = req.get_param_value("profile");
        } else if (req.has_header("X-Aimon-Profile")) {
            profile = req.get_header_value("X-Aimon-Profile");
        }

        std::string sessionId = generateSessionId();
        auto session = std::make_shared<SseSession>();
        session->id = sessionId;
        session->profile = profile;

        {
            std::lock_guard<std::mutex> lock(_sessionsMutex);
            _sseSessions[sessionId] = session;
        }

        std::string remoteIp = req.remote_addr;
        TaskRegistry::getInstance().registerSession(sessionId, remoteIp);

        std::cout << "[WebServer] New SSE client connected from " << remoteIp
                  << ", session: " << sessionId;
        if (!profile.empty()) {
            std::cout << ", profile: " << profile;
        }
        std::cout << std::endl;

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
                std::string endpointMsg = "event: endpoint\ndata: /message?sessionId=" + session->id + (!session->profile.empty() ? ("&profile=" + session->profile) : "") + "\n\n";
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
            TaskRegistry::getInstance().removeSession(sessionId);
            std::cout << "[WebServer] SSE client disconnected, session: " << sessionId << std::endl;
        };

        res.set_chunked_content_provider("text/event-stream", provider, releaser);
    });

    auto handleMcpMessage = [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization, Accept, Mcp-Session-Id, mcp-session-id, X-Aimon-Profile");
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

        // Check if an SSE session exists for push notification
        std::shared_ptr<SseSession> session;
        if (!sessionId.empty()) {
            std::lock_guard<std::mutex> lock(_sessionsMutex);
            auto it = _sseSessions.find(sessionId);
            if (it != _sseSessions.end()) {
                session = it->second;
            }
        }

        // Extract profile from query params, headers, or existing SSE session
        std::string profile;
        if (req.has_param("profile")) {
            profile = req.get_param_value("profile");
        } else if (req.has_header("X-Aimon-Profile")) {
            profile = req.get_header_value("X-Aimon-Profile");
        } else if (session && !session->profile.empty()) {
            profile = session->profile;
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
        std::cout << "[WebServer] MCP POST: method=" << method << " session='" << sessionId << "'";
        if (!profile.empty()) {
            std::cout << " profile='" << profile << "'";
        }
        std::cout << std::endl;

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

                // Auto-scope session profile from workspace if session has no explicit profile
                if (session && session->profile.empty()) {
                    std::string detected;
                    if (p.contains("rootUri") && p["rootUri"].is_string()) {
                        detected = McpServer::detectProfileFromWorkspace(p["rootUri"].get<std::string>());
                    }
                    if (detected.empty() && p.contains("rootPath") && p["rootPath"].is_string()) {
                        detected = McpServer::detectProfileFromWorkspace(p["rootPath"].get<std::string>());
                    }
                    if (detected.empty() && p.contains("workspaceFolders") && p["workspaceFolders"].is_array()) {
                        for (const auto& wf : p["workspaceFolders"]) {
                            if (wf.contains("uri") && wf["uri"].is_string()) {
                                detected = McpServer::detectProfileFromWorkspace(wf["uri"].get<std::string>());
                                if (!detected.empty()) break;
                            }
                        }
                    }
                    if (!detected.empty()) {
                        session->profile = detected;
                        profile = detected;  // update for this request too
                        std::cout << "[WebServer] Auto-scoped session " << sessionId
                                  << " profile to: " << detected << std::endl;
                    }
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

        // Process message through MCP server engine with profile filtering
        nlohmann::json respJson = _mcpServer->handleMessage(reqJson, profile);
        std::string respStr = respJson.is_null() ? "{}" : respJson.dump();

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

    _server->set_read_timeout(1, 0);
    _server->set_write_timeout(5, 0);

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
