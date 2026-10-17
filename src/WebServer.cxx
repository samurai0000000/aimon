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
#include "AgentTelemetryDb.hxx"
#include "MobileGateway.hxx"
#include "PathUtils.hxx"
#include "InsightEngine.hxx"
#include "EdaBridge.hxx"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <random>
#include <sstream>
#include <iomanip>
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#ifndef CPPHTTPLIB_THREAD_POOL_COUNT
#define CPPHTTPLIB_THREAD_POOL_COUNT 8
#endif
#include <httplib.h>

#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <algorithm>
#include <set>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

namespace fs = std::filesystem;

namespace aimon {

static std::string getPrimaryLanIp() {
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == -1) {
        return "";
    }
    std::string primaryIp;
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family == AF_INET) {
            if (!(ifa->ifa_flags & IFF_LOOPBACK) && (ifa->ifa_flags & IFF_UP)) {
                char ipStr[INET_ADDRSTRLEN];
                void* sin_addr = &((struct sockaddr_in*)ifa->ifa_addr)->sin_addr;
                if (inet_ntop(AF_INET, sin_addr, ipStr, sizeof(ipStr))) {
                    std::string ip(ipStr);
                    if (ip.rfind("192.168.", 0) == 0 || ip.rfind("10.", 0) == 0 || ip.rfind("172.", 0) == 0) {
                        primaryIp = ip;
                        break;
                    }
                    if (primaryIp.empty()) {
                        primaryIp = ip;
                    }
                }
            }
        }
    }
    freeifaddrs(ifaddr);
    return primaryIp;
}

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
      _mobileGateway(&MobileGateway::getInstance()),
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

std::string WebServer::extractMobileToken(const httplib::Request& req) {
    if (req.has_header("X-Mobile-Token")) {
        std::string token = req.get_header_value("X-Mobile-Token");
        while (!token.empty() && (token.back() == '\r' || token.back() == '\n' || token.back() == ' ')) {
            token.pop_back();
        }
        if (!token.empty()) {
            return token;
        }
    }
    if (req.has_header("Authorization")) {
        std::string auth = req.get_header_value("Authorization");
        if (auth.size() > 7) {
            std::string prefix = auth.substr(0, 7);
            for (char &c : prefix) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            if (prefix == "bearer ") {
                std::string token = auth.substr(7);
                size_t first = token.find_first_not_of(" \t\r\n");
                if (first != std::string::npos) {
                    size_t last = token.find_last_not_of(" \t\r\n");
                    return token.substr(first, last - first + 1);
                }
            }
        }
    }
    return "";
}

bool WebServer::isValidMobileToken(const httplib::Request& req) const {
    std::string token = extractMobileToken(req);
    if (token.empty() || !_mobileGateway) {
        return false;
    }
    return _mobileGateway->validateToken(token);
}

bool WebServer::isLoopbackRequest(const httplib::Request& req) {
    return req.remote_addr == "127.0.0.1" ||
           req.remote_addr == "::1" ||
           req.remote_addr == "::ffff:127.0.0.1";
}

bool WebServer::isValidUiSession(const httplib::Request& req) const {
    // 1. Check explicit Web UI session token via header or cookie
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

    if (!token.empty()) {
        std::lock_guard<std::mutex> lock(_sessionMutex);
        auto it = _uiSessions.find(token);
        if (it != _uiSessions.end()) {
            time_t now = time(nullptr);
            if (now - it->second < 86400 * 7) {
                return true;
            }
        }
    }

    // 2. Allow requests with valid paired mobile authentication tokens
    if (isValidMobileToken(req)) {
        return true;
    }

    // 3. Allow internal telemetry hook
    if (req.has_header("User-Agent")) {
        std::string ua = req.get_header_value("User-Agent");
        if (ua.find("aimon-hook") != std::string::npos && isLoopbackRequest(req)) {
            return true;
        }
    }

    return false;
}

bool WebServer::isValidMobileOrUiRequest(const httplib::Request& req) const {
    return isValidMobileToken(req) || isValidUiSession(req);
}

std::string WebServer::getMcpHintForPath(const std::string& path, const std::string& body) {
    (void)body;
    if (path == "/api/status" || path == "/api/history") {
        return "Use official MCP tool 'get_combined_ai_status', 'check_antigravity_quota', or 'check_cursor_usage'.";
    }
    return "Use official MCP tools ('aimon' gateway or native tools). Direct REST API access is disabled.";
}

void WebServer::setupRoutes() {
    // Connect MobileGateway broadcast callback to broadcast SSE events
    if (_mobileGateway) {
        _mobileGateway->setBroadcastCallback([this](const std::string& type, const nlohmann::json& payload) {
            nlohmann::json evt;
            evt["type"] = type;
            evt["payload"] = payload;
            broadcastSseNotification(evt.dump());
        });
    }

    // Enforce authentication on mobile, approval, agent, and workspace routes,
    // and gate direct REST API endpoints when disabled by configuration.
    _server->set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
        if (req.method == "OPTIONS") {
            return httplib::Server::HandlerResponse::Unhandled;
        }

        // Pairing endpoint exchanges a one-time pairing secret for a token
        if (req.path == "/api/mobile/pair") {
            return httplib::Server::HandlerResponse::Unhandled;
        }

        // QR generation requires authenticated Web UI session or loopback
        if (req.path == "/api/mobile/qr") {
            if (!isValidUiSession(req) && !isLoopbackRequest(req)) {
                res.status = 401;
                res.set_header("Access-Control-Allow-Origin", "*");
                res.set_content("{\"error\":\"Unauthorized\"}", "application/json");
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        }

        // Local CLI hooks on loopback or authenticated mobile/UI clients may submit/poll approval requests
        if (req.path == "/api/approvals/request" || req.path == "/api/approvals/status") {
            if (!isLoopbackRequest(req) && !isValidMobileOrUiRequest(req)) {
                res.status = 401;
                res.set_header("Access-Control-Allow-Origin", "*");
                res.set_content("{\"error\":\"Unauthorized\"}", "application/json");
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        }

        // Protected mobile, approval, agent, workspace, and EDA mutation endpoints require a valid token or UI session
        if (req.path == "/api/approvals/pending" ||
            req.path == "/api/approvals/decision" ||
            req.path == "/api/mobile/devices" ||
            req.path == "/api/mobile/revoke" ||
            req.path == "/api/agent" ||
            req.path.rfind("/api/agent/", 0) == 0 ||
            req.path == "/api/workspace" ||
            req.path.rfind("/api/workspace/", 0) == 0 ||
            req.path == "/api/eda/config" ||
            req.path == "/api/eda/probe") {
            if (!isValidMobileOrUiRequest(req)) {
                res.status = 401;
                res.set_header("Access-Control-Allow-Origin", "*");
                res.set_content("{\"error\":\"Unauthorized\"}", "application/json");
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        }

        // Mobile SSE push stream requires a valid mobile token or UI session
        if (req.path == "/sse") {
            std::string profile;
            if (req.has_param("profile")) {
                profile = req.get_param_value("profile");
            } else if (req.has_header("X-Aimon-Profile")) {
                profile = req.get_header_value("X-Aimon-Profile");
            }
            if (profile == "mobile" && !isValidMobileOrUiRequest(req)) {
                res.status = 401;
                res.set_header("Access-Control-Allow-Origin", "*");
                res.set_content("{\"error\":\"Unauthorized\"}", "application/json");
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        }

        if (!_endpointsEnabled) {
            if (req.path == "/api/monitors" ||
                req.path == "/api/status" ||
                req.path.rfind("/api/telemetry", 0) == 0 ||
                req.path.rfind("/api/v1/insights", 0) == 0) {
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
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });


    auto serveFileOrFallback = [](const std::string& diskPath,
                                  const char* fallbackAsset,
                                  const std::string& contentType,
                                  httplib::Response& res) {
        res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
        res.set_header("Pragma", "no-cache");
        res.set_header("Expires", "0");
        if (fs::exists(diskPath)) {
            std::ifstream f(diskPath, std::ios::binary);
            if (f.is_open()) {
                std::string content((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
                res.set_content(content, contentType.c_str());
                return;
            }
        }
        if (fallbackAsset && fallbackAsset[0] != '\0') {
            res.set_content(fallbackAsset, contentType.c_str());
        } else {
            res.status = 404;
            res.set_content("File not found", "text/plain");
        }
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

    _server->Get("/qrcode.js", [serveFileOrFallback](const httplib::Request&, httplib::Response& res) {
        serveFileOrFallback("web/qrcode.js", assets::QRCODE_JS, "application/javascript", res);
    });

    _server->Get("/download/aimon-companion.apk", [serveFileOrFallback](const httplib::Request&, httplib::Response& res) {
        res.set_header("Content-Disposition", "attachment; filename=\"aimon-companion.apk\"");
        if (fs::exists("third_party/antimatter/android/app/build/outputs/apk/debug/app-debug.apk")) {
            serveFileOrFallback("third_party/antimatter/android/app/build/outputs/apk/debug/app-debug.apk", "", "application/vnd.android.package-archive", res);
        } else {
            serveFileOrFallback("mobile/android/app/build/outputs/apk/debug/app-debug.apk", "", "application/vnd.android.package-archive", res);
        }
    });

    _server->Get("/favicon.ico", [](const httplib::Request&, httplib::Response& res) {
        static const std::string faviconSvg =
            "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 32 32\">"
            "<circle cx=\"16\" cy=\"16\" r=\"14\" fill=\"#0a0e17\" stroke=\"#00f2fe\" stroke-width=\"2\"/>"
            "<circle cx=\"16\" cy=\"16\" r=\"6\" fill=\"#00f2fe\"/>"
            "<circle cx=\"16\" cy=\"16\" r=\"10\" fill=\"none\" stroke=\"#8b5cf6\" stroke-width=\"1\" stroke-dasharray=\"2,2\"/>"
            "</svg>";
        res.set_header("Cache-Control", "public, max-age=86400");
        res.set_content(faviconSvg, "image/svg+xml");
    });

    _server->Get("/api/status", [this](const httplib::Request&, httplib::Response& res) {
        AggregateStatus st = _stateStore.getStatus();
        if (_edaBridge) {
            st.eda = _edaBridge->getStatus();
        }
        if (st.eda.configSnapshot.is_object()) {
            st.eda.configSnapshot["auth_secret"] = st.eda.authConfigured ? "***" : "";
        }
        std::string jsonStr = st.toJson().dump(2);
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
        AggregateStatus st = _stateStore.getStatus();
        if (_edaBridge) {
            st.eda = _edaBridge->getStatus();
        }
        if (st.eda.configSnapshot.is_object()) {
            st.eda.configSnapshot["auth_secret"] = st.eda.authConfigured ? "***" : "";
        }
        std::string jsonStr = st.toJson().dump(2);
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

    // --- Telemetry Endpoints ---

    _server->Post("/api/telemetry/event", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        try {
            nlohmann::json body = nlohmann::json::parse(req.body);
            std::string eventType = body.value("event_type", "");
            std::string sessionId = body.value("session_id", "default");
            std::string agentType = body.value("agent_type", "antigravity");
            int64_t ts = body.value("timestamp", static_cast<int64_t>(time(nullptr)));

            if (eventType == "SESSION_START") {
                std::string convId = body.value("conversation_id", sessionId);
                std::string workspace = body.value("workspace", "");
                std::string model = body.value("model", "default");
                AgentTelemetryDb::getInstance().recordSessionStart(sessionId, convId, agentType, workspace, model, ts);
            } else if (eventType == "SESSION_END") {
                std::string status = body.value("status", "COMPLETED");
                int turns = body.value("total_turns", 0);
                int promptTokens = body.value("prompt_tokens", 0);
                int compTokens = body.value("comp_tokens", 0);
                int toolCalls = body.value("tool_calls", 0);
                int errors = body.value("errors", 0);
                double avgTurnMs = body.value("avg_turn_ms", 0.0);
                AgentTelemetryDb::getInstance().recordSessionEnd(sessionId, status, turns, promptTokens, compTokens, toolCalls, errors, avgTurnMs, ts);
            } else if (eventType == "sample" || eventType == "SAMPLE" || eventType == "TELEMETRY_SAMPLE") {
                AgentTelemetrySample sample;
                sample.timestamp = ts;
                sample.activeAgents = body.value("active_sessions", body.value("active_agents", 0));
                sample.promptTokensSec = body.value("prompt_tokens_per_sec", body.value("prompt_tokens_sec", 0.0));
                sample.compTokensSec = body.value("completion_tokens_per_sec", body.value("comp_tokens_sec", 0.0));
                sample.toolCallsSec = body.value("tool_calls_per_sec", body.value("tool_calls_sec", 0.0));
                sample.errorRatePct = body.value("tool_error_rate_pct", body.value("error_rate_pct", 0.0));
                sample.avgTurnLatencyMs = body.value("turn_latency_ms", body.value("avg_turn_latency_ms", 0.0));
                sample.p95TurnLatencyMs = body.value("turn_latency_p95_ms", body.value("p95_turn_latency_ms", 0.0));
                sample.approvalWaitMs = body.value("approval_wait_latency_ms", body.value("approval_wait_ms", 0.0));
                AgentTelemetryDb::getInstance().insertSample(sample);
            } else {
                AgentLifecycleEvent ev;
                ev.timestamp = ts;
                ev.sessionId = sessionId;
                ev.agentType = agentType;
                ev.eventType = eventType;
                ev.stepIndex = body.value("step_index", 0);
                ev.toolName = body.value("tool_name", "");
                ev.durationMs = body.value("duration_ms", 0.0);
                ev.status = body.value("status", "OK");
                if (body.contains("details")) {
                    ev.detailsJson = body["details"];
                }
                AgentTelemetryDb::getInstance().insertEvent(ev);
            }

            res.set_content("{\"status\":\"ok\"}", "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(nlohmann::json({{"error", e.what()}}).dump(), "application/json");
        }
    });

    _server->Get("/api/telemetry/overview", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        int hours = 24;
        if (req.has_param("hours")) {
            try { hours = std::stoi(req.get_param_value("hours")); } catch (...) {}
        }
        auto data = AgentTelemetryDb::getInstance().queryOverview(hours);
        res.set_content(data.dump(2), "application/json");
    });

    _server->Get("/api/telemetry/timeseries", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        std::string window = "24h";
        int maxPoints = 300;
        if (req.has_param("window")) {
            window = req.get_param_value("window");
        }
        if (req.has_param("max_points")) {
            try { maxPoints = std::stoi(req.get_param_value("max_points")); } catch (...) {}
        }
        auto data = AgentTelemetryDb::getInstance().queryTimeseries(window, maxPoints);
        res.set_content(data.dump(2), "application/json");
    });

    _server->Get("/api/telemetry/activity_timeline", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        std::string window = "24h";
        int maxSessions = 50;
        if (req.has_param("window")) {
            window = req.get_param_value("window");
        }
        if (req.has_param("max_sessions")) {
            try { maxSessions = std::stoi(req.get_param_value("max_sessions")); } catch (...) {}
        }
        auto data = AgentTelemetryDb::getInstance().queryActivityTimeline(window, maxSessions);
        res.set_content(data.dump(2), "application/json");
    });

    _server->Get("/api/telemetry/sessions", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        int limit = 50;
        std::string status = "";
        if (req.has_param("limit")) {
            try { limit = std::stoi(req.get_param_value("limit")); } catch (...) {}
        }
        if (req.has_param("status")) {
            status = req.get_param_value("status");
        }
        auto data = AgentTelemetryDb::getInstance().querySessions(limit, status);
        res.set_content(data.dump(2), "application/json");
    });

    _server->Get("/api/telemetry/session_events", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        std::string sessionId = req.has_param("sessionId") ? req.get_param_value("sessionId") :
                               (req.has_param("session_id") ? req.get_param_value("session_id") : "");
        auto data = AgentTelemetryDb::getInstance().querySessionEvents(sessionId);
        res.set_content(data.dump(2), "application/json");
    });

    _server->Get("/api/telemetry/tools", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        int hours = 24;
        if (req.has_param("hours")) {
            try { hours = std::stoi(req.get_param_value("hours")); } catch (...) {}
        }
        auto data = AgentTelemetryDb::getInstance().queryToolStats(hours);
        res.set_content(data.dump(2), "application/json");
    });

    // --- Action Approval Endpoints ---

    _server->Post("/api/approvals/request", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        if (!isLoopbackRequest(req) && !isValidMobileOrUiRequest(req)) {
            res.status = 401;
            res.set_content("{\"error\":\"Unauthorized\"}", "application/json");
            return;
        }
        try {
            nlohmann::json body = nlohmann::json::parse(req.body);
            std::string agentType = body.value("agent_type", body.value("agent", "antigravity"));
            std::string toolName = body.value("tool_name", body.value("action_type", "run_command"));
            std::string workspace = body.value("workspace", "");
            nlohmann::json toolArgs = body.value("tool_args", nlohmann::json::object());
            if (body.contains("command") && !toolArgs.contains("CommandLine")) {
                toolArgs["CommandLine"] = body["command"];
            }
            if (body.contains("details") && toolArgs.empty() && body["details"].is_object()) {
                toolArgs = body["details"];
            }
            std::string reason = body.value("reason", "");
            int timeoutSec = body.value("timeout_seconds", 120);
            bool waitSync = body.value("wait", !body.contains("action_type") && !body.value("async", false));

            MobileGateway& gw = _mobileGateway ? *_mobileGateway : MobileGateway::getInstance();
            std::string approvalId = gw.submitApprovalRequest(
                agentType, toolName, workspace, toolArgs, reason, timeoutSec);

            if (!waitSync) {
                nlohmann::json resp;
                resp["id"] = approvalId;
                resp["request_id"] = approvalId;
                resp["approval_id"] = approvalId;
                resp["status"] = "pending";
                resp["verdict"] = "PENDING";
                res.set_content(resp.dump(2), "application/json");
                return;
            }

            // Block and wait for mobile / UI approval
            ApprovalVerdict verdict = gw.waitForApproval(approvalId, timeoutSec);
            std::string verdictStr = (verdict == ApprovalVerdict::APPROVED) ? "APPROVED" :
                                     (verdict == ApprovalVerdict::DENIED) ? "DENIED" : "TIMED_OUT";
            std::string statusStr = (verdict == ApprovalVerdict::APPROVED) ? "approved" :
                                    (verdict == ApprovalVerdict::DENIED) ? "denied" : "expired";

            nlohmann::json resp;
            resp["id"] = approvalId;
            resp["request_id"] = approvalId;
            resp["approval_id"] = approvalId;
            resp["status"] = statusStr;
            resp["verdict"] = verdictStr;
            res.set_content(resp.dump(2), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(nlohmann::json({{"error", e.what()}}).dump(), "application/json");
        }
    });

    _server->Get("/api/approvals/status", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        if (!isLoopbackRequest(req) && !isValidMobileOrUiRequest(req)) {
            res.status = 401;
            res.set_content("{\"error\":\"Unauthorized\"}", "application/json");
            return;
        }
        std::string id;
        if (req.has_param("id")) {
            id = req.get_param_value("id");
        } else if (req.has_param("approval_id")) {
            id = req.get_param_value("approval_id");
        } else if (req.has_param("request_id")) {
            id = req.get_param_value("request_id");
        }
        if (id.empty()) {
            res.status = 400;
            res.set_content("{\"error\":\"Missing approval id\"}", "application/json");
            return;
        }
        MobileGateway& gw = _mobileGateway ? *_mobileGateway : MobileGateway::getInstance();
        auto st = gw.getApprovalStatus(id);
        res.set_content(st.dump(2), "application/json");
    });

    _server->Post("/api/approvals/decision", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        if (!isValidMobileOrUiRequest(req)) {
            res.status = 401;
            res.set_content("{\"error\":\"Unauthorized\"}", "application/json");
            return;
        }
        try {
            nlohmann::json body = nlohmann::json::parse(req.body);
            std::string approvalId = body.value("approval_id", body.value("id", ""));
            std::string decision = body.value("decision", "deny");

            std::string lower = decision;
            for (char &c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

            ApprovalVerdict verdict = (lower == "allow" || lower == "approve" || lower == "approved")
                ? ApprovalVerdict::APPROVED : ApprovalVerdict::DENIED;

            MobileGateway& gw = _mobileGateway ? *_mobileGateway : MobileGateway::getInstance();
            bool resolved = gw.resolveApproval(approvalId, verdict);
            nlohmann::json resp;
            resp["approval_id"] = approvalId;
            resp["resolved"] = resolved;
            res.set_content(resp.dump(2), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(nlohmann::json({{"error", e.what()}}).dump(), "application/json");
        }
    });

    _server->Get("/api/approvals/pending", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        if (!isValidMobileOrUiRequest(req)) {
            res.status = 401;
            res.set_content("{\"error\":\"Unauthorized\"}", "application/json");
            return;
        }
        MobileGateway& gw = _mobileGateway ? *_mobileGateway : MobileGateway::getInstance();
        auto pending = gw.listPendingApprovals();
        nlohmann::json arr = pending;
        res.set_content(arr.dump(2), "application/json");
    });

    // --- Mobile Gateway & Pairing Endpoints ---

    _server->Get("/api/mobile/qr", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        if (!isValidUiSession(req) && !isLoopbackRequest(req)) {
            res.status = 401;
            res.set_content("{\"error\":\"Unauthorized\"}", "application/json");
            return;
        }
        MobileGateway& gw = _mobileGateway ? *_mobileGateway : MobileGateway::getInstance();
        std::string secret = gw.createPairingSecret(300);
        std::string lanIp = getPrimaryLanIp();

        nlohmann::json resp;
        resp["secret"] = secret;
        resp["expires_in"] = 300;
        if (!lanIp.empty()) {
            resp["lan_ip"] = lanIp;
        }
        res.set_content(resp.dump(2), "application/json");
    });

    _server->Post("/api/mobile/pair", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        try {
            nlohmann::json body = nlohmann::json::parse(req.body);
            std::string secret = body.value("secret", "");
            std::string deviceId = body.value("device_id", "");
            std::string deviceName = body.value("device_name", "");

            MobileGateway& gw = _mobileGateway ? *_mobileGateway : MobileGateway::getInstance();
            std::string token;
            if (gw.pairDevice(secret, deviceId, deviceName, token)) {
                std::string resolvedDevId = deviceId;
                gw.authenticate(token, resolvedDevId);
                nlohmann::json resp;
                resp["token"] = token;
                resp["device_id"] = resolvedDevId;
                resp["status"] = "paired";
                res.set_content(resp.dump(2), "application/json");
            } else {
                res.status = 401;
                res.set_content("{\"error\":\"Invalid or expired pairing secret\"}", "application/json");
            }
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(nlohmann::json({{"error", e.what()}}).dump(), "application/json");
        }
    });

    _server->Get("/api/mobile/devices", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        if (!isValidMobileOrUiRequest(req)) {
            res.status = 401;
            res.set_content("{\"error\":\"Unauthorized\"}", "application/json");
            return;
        }
        MobileGateway& gw = _mobileGateway ? *_mobileGateway : MobileGateway::getInstance();
        auto devs = gw.listDevices();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& d : devs) {
            arr.push_back({
                {"device_id", d.deviceId},
                {"device_name", d.deviceName},
                {"paired_at", d.pairedAt},
                {"last_seen_at", d.lastSeenAt}
            });
        }
        res.set_content(arr.dump(2), "application/json");
    });

    _server->Post("/api/mobile/revoke", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        if (!isValidMobileOrUiRequest(req)) {
            res.status = 401;
            res.set_content("{\"error\":\"Unauthorized\"}", "application/json");
            return;
        }
        try {
            nlohmann::json body = nlohmann::json::parse(req.body);
            std::string deviceId = body.value("device_id", "");
            MobileGateway& gw = _mobileGateway ? *_mobileGateway : MobileGateway::getInstance();
            gw.revokeDevice(deviceId);
            res.set_content("{\"status\":\"revoked\"}", "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(nlohmann::json({{"error", e.what()}}).dump(), "application/json");
        }
    });

    // --- Zero-Python Native Antimatter Agent & Workspace Bridge Endpoints ---

    auto getWorkspaceRoot = []() -> fs::path {
        const char* envWs = std::getenv("AIMON_WORKSPACE_ROOT");
        if (envWs && *envWs) {
            return fs::path(envWs);
        }
        return fs::current_path();
    };

    auto getIdeBrainDir = []() -> std::string {
        const char* envIde = std::getenv("AIMON_IDE_BRAIN_DIR");
        if (envIde && *envIde) {
            return std::string(envIde);
        }
        return PathUtils::expandHome("~/.gemini/antigravity-ide/brain");
    };

    auto getCliBrainDir = []() -> std::string {
        const char* envCli = std::getenv("AIMON_CLI_BRAIN_DIR");
        if (envCli && *envCli) {
            return std::string(envCli);
        }
        return PathUtils::expandHome("~/.gemini/antigravity-cli/brain");
    };

    auto getCliPresenceDir = []() -> std::string {
        const char* envPres = std::getenv("AIMON_CLI_PRESENCE_DIR");
        if (envPres && *envPres) {
            return std::string(envPres);
        }
        return PathUtils::expandHome("~/.gemini/antigravity-cli/presence");
    };

    auto stripUserRequestTags = [](const std::string& raw) -> std::string {
        std::string text = raw;
        auto startTag = text.find("<USER_REQUEST>");
        auto endTag = text.find("</USER_REQUEST>");
        if (startTag != std::string::npos && endTag != std::string::npos && endTag >= startTag + 14) {
            text = text.substr(startTag + 14, endTag - (startTag + 14));
        }
        auto first = text.find_first_not_of(" \n\r\t");
        if (first == std::string::npos) {
            return "";
        }
        auto last = text.find_last_not_of(" \n\r\t");
        return text.substr(first, last - first + 1);
    };

    auto handleAgentAvailable = [getWorkspaceRoot](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        std::string wsRoot = getWorkspaceRoot().string();
        nlohmann::json resp;
        resp["agents"] = nlohmann::json::array({
            {
                {"id", "antigravity"},
                {"name", "Google Antigravity"},
                {"status", "online"},
                {"workspaceRoot", wsRoot}
            }
        });
        resp["allowed_workspaces"] = nlohmann::json::array({wsRoot});
        resp["current_workspace"] = wsRoot;
        res.set_content(resp.dump(2), "application/json");
    };
    _server->Get("/api/agent/available", handleAgentAvailable);
    _server->Get("/api/agent/agents", handleAgentAvailable);

    auto handleAgentConversations = [getIdeBrainDir, getCliBrainDir, getCliPresenceDir, stripUserRequestTags](
                                        const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        size_t limit = 50;
        if (req.has_param("limit")) {
            try {
                int parsed = std::stoi(req.get_param_value("limit"));
                if (parsed > 0) {
                    limit = static_cast<size_t>(parsed);
                }
            } catch (...) {}
        }

        struct ConvEntry {
            std::string id;
            std::string title;
            std::string source;
            uint64_t timestamp = 0;
            bool active = false;
        };

        std::vector<ConvEntry> collected;
        std::string presenceDir = getCliPresenceDir();
        uint64_t nowSec = static_cast<uint64_t>(std::time(nullptr));

        auto scanRoot = [&](const std::string& rootPath, const std::string& sourceLabel) {
            if (rootPath.empty() || !fs::exists(rootPath) || !fs::is_directory(rootPath)) {
                return;
            }
            std::error_code ec;
            for (const auto& entry : fs::directory_iterator(rootPath, ec)) {
                if (ec || !entry.is_directory()) continue;
                std::string id = entry.path().filename().string();
                if (id.empty() || id[0] == '.' || id == "tempmediaStorage") continue;

                fs::path transcriptPath = entry.path() / ".system_generated" / "logs" / "transcript.jsonl";
                fs::path taskMdPath = entry.path() / "task.md";
                bool hasTranscript = fs::exists(transcriptPath);
                bool hasTaskMd = fs::exists(taskMdPath);
                if (!hasTranscript && !hasTaskMd) continue;

                uint64_t ts = 0;
                try {
                    auto ftime = hasTranscript ? fs::last_write_time(transcriptPath)
                                               : fs::last_write_time(entry.path());
                    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                        ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now()
                    );
                    auto secs = std::chrono::duration_cast<std::chrono::seconds>(sctp.time_since_epoch()).count();
                    if (secs > 0) {
                        ts = static_cast<uint64_t>(secs);
                    }
                } catch (...) {}

                std::string title;
                if (hasTaskMd) {
                    std::ifstream tf(taskMdPath);
                    std::string tline;
                    while (std::getline(tf, tline)) {
                        auto first = tline.find_first_not_of(" \t\r\n");
                        if (first == std::string::npos) continue;
                        if (tline.compare(first, 2, "# ") == 0) {
                            title = tline.substr(first + 2);
                            auto last = title.find_last_not_of(" \t\r\n");
                            if (last != std::string::npos) {
                                title.erase(last + 1);
                            }
                            if (title.rfind("Task:", 0) == 0 || title.rfind("Plan:", 0) == 0) {
                                auto afterColon = title.find_first_not_of(" \t", 5);
                                if (afterColon != std::string::npos) {
                                    title = title.substr(afterColon);
                                }
                            }
                            break;
                        }
                    }
                }

                bool isInternalSubagent = false;
                if (title.empty() && hasTranscript) {
                    std::ifstream f(transcriptPath);
                    std::string line;
                    int scannedLines = 0;
                    while (std::getline(f, line) && scannedLines < 25) {
                        ++scannedLines;
                        if (line.empty()) continue;
                        try {
                            auto j = nlohmann::json::parse(line, nullptr, false);
                            if (j.is_discarded() || !j.is_object()) continue;
                            std::string stype = j.value("type", "");
                            if (scannedLines == 1 && stype == "SYSTEM_MESSAGE" && !hasTaskMd) {
                                isInternalSubagent = true;
                                break;
                            }
                            if (stype == "USER_INPUT" || (scannedLines == 1 && j.contains("content"))) {
                                std::string content = j.value("content", "");
                                std::string clean = stripUserRequestTags(content);
                                if (!clean.empty()) {
                                    auto nlPos = clean.find_first_of("\r\n");
                                    if (nlPos != std::string::npos) {
                                        clean = clean.substr(0, nlPos);
                                    }
                                    title = clean;
                                    if (stype == "USER_INPUT") {
                                        break;
                                    }
                                }
                            }
                        } catch (...) {}
                    }
                }
                if (isInternalSubagent) {
                    continue;
                }

                if (title.empty()) {
                    title = "Conversation " + id.substr(0, std::min<size_t>(8, id.length()));
                } else if (title.length() > 80) {
                    title = title.substr(0, 77) + "...";
                }

                bool isActive = false;
                if (!presenceDir.empty() && fs::exists(fs::path(presenceDir) / (id + ".lock"))) {
                    isActive = true;
                } else if (ts > 0 && nowSec >= ts && (nowSec - ts) < 120) {
                    isActive = true;
                }

                ConvEntry ce;
                ce.id = id;
                ce.title = title;
                ce.source = sourceLabel;
                ce.timestamp = ts;
                ce.active = isActive;
                collected.push_back(std::move(ce));
            }
        };

        scanRoot(getIdeBrainDir(), "ide");
        scanRoot(getCliBrainDir(), "cli");

        std::sort(collected.begin(), collected.end(), [](const ConvEntry& a, const ConvEntry& b) {
            if (a.timestamp != b.timestamp) {
                return a.timestamp > b.timestamp;
            }
            return a.id > b.id;
        });

        nlohmann::json conversations = nlohmann::json::array();
        size_t count = std::min(collected.size(), limit);
        for (size_t i = 0; i < count; ++i) {
            conversations.push_back({
                {"id", collected[i].id},
                {"title", collected[i].title},
                {"source", collected[i].source},
                {"timestamp", collected[i].timestamp},
                {"active", collected[i].active}
            });
        }

        nlohmann::json resp;
        resp["conversations"] = conversations;
        res.set_content(resp.dump(2), "application/json");
    };
    _server->Get("/api/agent/conversations", handleAgentConversations);
    _server->Get("/api/agent/history", handleAgentConversations);

    struct LsEndpoint {
        int port = 0;
        std::string csrf;
        bool isPreferredWorkspace = false;
    };

    auto discoverLsEndpoints = []() -> std::vector<LsEndpoint> {
        std::vector<LsEndpoint> endpoints;

        auto addEndpoint = [&](int port, const std::string& csrf, bool preferred = false) {
            if (port <= 0 || port > 65535 || csrf.empty()) return;
            for (auto& ep : endpoints) {
                if (ep.port == port && ep.csrf == csrf) {
                    if (preferred) ep.isPreferredWorkspace = true;
                    return;
                }
            }
            endpoints.push_back({port, csrf, preferred});
        };

        const char* envLsPort = std::getenv("AGY_LS_PORT");
        const char* envLsCsrf = std::getenv("AGY_LS_CSRF_TOKEN");
        const char* envAgAddr = std::getenv("ANTIGRAVITY_LS_ADDRESS");
        const char* envAgCsrf = std::getenv("ANTIGRAVITY_CSRF_TOKEN");
        std::string fallbackCsrf = (envLsCsrf && *envLsCsrf) ? envLsCsrf :
                                   ((envAgCsrf && *envAgCsrf) ? envAgCsrf : "");
        if (envLsPort && *envLsPort) {
            try {
                addEndpoint(std::stoi(envLsPort), fallbackCsrf, true);
            } catch (...) {}
        }
        if (envAgAddr && *envAgAddr) {
            std::string addr(envAgAddr);
            auto colon = addr.rfind(':');
            if (colon != std::string::npos) {
                try {
                    addEndpoint(std::stoi(addr.substr(colon + 1)), fallbackCsrf, true);
                } catch (...) {}
            }
        }

        if (endpoints.empty() && fs::exists("/proc")) {
            std::vector<pid_t> pids;
            std::error_code ec;
            for (const auto& entry : fs::directory_iterator("/proc", ec)) {
                if (ec || !entry.is_directory()) continue;
                std::string dname = entry.path().filename().string();
                if (!dname.empty() && std::all_of(dname.begin(), dname.end(), ::isdigit)) {
                    try { pids.push_back(std::stoi(dname)); } catch (...) {}
                }
            }
            std::sort(pids.begin(), pids.end(), std::greater<pid_t>());

            for (pid_t pid : pids) {
                std::ifstream envFile("/proc/" + std::to_string(pid) + "/environ", std::ios::binary);
                if (!envFile.is_open()) continue;
                std::string envContent((std::istreambuf_iterator<char>(envFile)),
                                       std::istreambuf_iterator<char>());
                if (envContent.find("ANTIGRAVITY_LS_ADDRESS=") == std::string::npos) continue;
                std::string lsAddr;
                std::string lsCsrf;
                size_t pos = 0;
                while (pos < envContent.size()) {
                    std::string item = envContent.c_str() + pos;
                    if (item.rfind("ANTIGRAVITY_LS_ADDRESS=", 0) == 0) {
                        lsAddr = item.substr(23);
                    } else if (item.rfind("ANTIGRAVITY_CSRF_TOKEN=", 0) == 0) {
                        lsCsrf = item.substr(23);
                    }
                    pos += item.length() + 1;
                }
                if (!lsAddr.empty() && !lsCsrf.empty()) {
                    auto colon = lsAddr.rfind(':');
                    if (colon != std::string::npos) {
                        bool preferred = false;
                        std::error_code cwdEc;
                        auto cwdPath = fs::read_symlink("/proc/" + std::to_string(pid) + "/cwd", cwdEc);
                        if (!cwdEc && cwdPath.string().find("aimon") != std::string::npos) {
                            preferred = true;
                        }
                        try {
                            addEndpoint(std::stoi(lsAddr.substr(colon + 1)), lsCsrf, preferred);
                        } catch (...) {}
                    }
                }
            }

            auto findPortsForPid = [](pid_t pid) -> std::vector<int> {
                std::vector<int> ports;
                std::set<std::string> inodes;
                std::string fdDir = "/proc/" + std::to_string(pid) + "/fd";
                std::error_code fdEc;
                if (!fs::exists(fdDir, fdEc)) return ports;
                for (const auto& fdEntry : fs::directory_iterator(fdDir, fdEc)) {
                    if (fdEc) break;
                    std::error_code symEc;
                    if (fs::is_symlink(fdEntry.path(), symEc)) {
                        std::string target = fs::read_symlink(fdEntry.path(), symEc).string();
                        if (!symEc && target.rfind("socket:[", 0) == 0 && target.size() > 9) {
                            inodes.insert(target.substr(8, target.size() - 9));
                        }
                    }
                }
                if (inodes.empty()) return ports;

                auto scanNetFile = [&](const std::string& netPath) {
                    std::ifstream nf(netPath);
                    std::string line;
                    std::getline(nf, line);
                    while (std::getline(nf, line)) {
                        std::istringstream ss(line);
                        std::string sl, localAddr, remAddr, state, txRx, trTm, retrnsmt, uid, timeout, inode;
                        if (ss >> sl >> localAddr >> remAddr >> state >> txRx >> trTm >> retrnsmt >> uid >> timeout >> inode) {
                            if (state == "0A" && inodes.count(inode)) {
                                auto colon = localAddr.find(':');
                                if (colon != std::string::npos) {
                                    try {
                                        int p = std::stoi(localAddr.substr(colon + 1), nullptr, 16);
                                        if (p > 0) ports.push_back(p);
                                    } catch (...) {}
                                }
                            }
                        }
                    }
                };
                scanNetFile("/proc/net/tcp");
                scanNetFile("/proc/net/tcp6");
                return ports;
            };

            for (pid_t pid : pids) {
                std::ifstream cmdFile("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
                if (!cmdFile.is_open()) continue;
                std::string cmdContent((std::istreambuf_iterator<char>(cmdFile)),
                                       std::istreambuf_iterator<char>());
                if (cmdContent.find("language_server") == std::string::npos) continue;

                std::string csrf;
                std::string workspaceId;
                size_t pos = 0;
                while (pos < cmdContent.size()) {
                    std::string arg = cmdContent.c_str() + pos;
                    if (arg == "--csrf_token" && (pos + arg.length() + 1) < cmdContent.size()) {
                        csrf = cmdContent.c_str() + pos + arg.length() + 1;
                    } else if (arg == "--workspace_id" && (pos + arg.length() + 1) < cmdContent.size()) {
                        workspaceId = cmdContent.c_str() + pos + arg.length() + 1;
                    }
                    pos += arg.length() + 1;
                }
                if (csrf.empty()) csrf = fallbackCsrf;
                if (csrf.empty()) continue;

                bool preferred = (workspaceId.find("aimon") != std::string::npos);
                for (int p : findPortsForPid(pid)) {
                    addEndpoint(p, csrf, preferred);
                }
            }
        }

        std::stable_sort(endpoints.begin(), endpoints.end(), [](const LsEndpoint& a, const LsEndpoint& b) {
            return a.isPreferredWorkspace > b.isPreferredWorkspace;
        });
        return endpoints;
    };

    auto lastRpcEndpoint = std::make_shared<std::pair<int, std::string>>(0, "");
    auto lastRpcMutex = std::make_shared<std::mutex>();

    auto mapModelPlaceholderToName = [](const std::string& raw) -> std::string {
        if (raw == "MODEL_PLACEHOLDER_M418" || raw == "gemini-4-argon-medium" || raw == "pro") {
            return "gemini-4-argon-medium";
        }
        if (raw == "MODEL_PLACEHOLDER_M322" || raw == "gemini-3.8-flash-tiered" || raw == "flash") {
            return "gemini-3.8-flash-tiered";
        }
        return raw;
    };

    auto extractTrajectoryModelName = [mapModelPlaceholderToName](const nlohmann::json& tj) -> std::string {
        if (!tj.is_object() || !tj.contains("trajectory") || !tj["trajectory"].is_object()) {
            return "gemini-4-argon-medium";
        }
        const auto& traj = tj["trajectory"];
        std::string fallbackM322Model;
        if (traj.contains("executorMetadatas") && traj["executorMetadatas"].is_array()) {
            const auto& ems = traj["executorMetadatas"];
            for (auto it = ems.rbegin(); it != ems.rend(); ++it) {
                if (!it->is_object() || !it->contains("cascadeConfig") || !(*it)["cascadeConfig"].is_object()) {
                    continue;
                }
                const auto& cc = (*it)["cascadeConfig"];
                if (!cc.contains("plannerConfig") || !cc["plannerConfig"].is_object()) {
                    continue;
                }
                const auto& pc = cc["plannerConfig"];
                std::string mName = pc.value("modelName", "");
                std::string pModel = pc.value("planModel", "");
                bool hasCustomCfg = pc.contains("customizationConfig") && !pc["customizationConfig"].is_null() &&
                                    pc["customizationConfig"].is_object() && !pc["customizationConfig"].empty();
                std::string candidate = !mName.empty() ? mapModelPlaceholderToName(mName)
                                                       : (!pModel.empty() ? mapModelPlaceholderToName(pModel) : "");
                if (candidate.empty()) {
                    continue;
                }
                if (candidate == "gemini-3.8-flash-tiered" && !hasCustomCfg) {
                    if (fallbackM322Model.empty()) {
                        fallbackM322Model = candidate;
                    }
                    continue;
                }
                return candidate;
            }
        }
        if (traj.contains("generatorMetadata") && traj["generatorMetadata"].is_array()) {
            const auto& gms = traj["generatorMetadata"];
            for (auto it = gms.rbegin(); it != gms.rend(); ++it) {
                if (it->is_object() && it->contains("chatModel") && (*it)["chatModel"].is_object()) {
                    std::string gmModel = (*it)["chatModel"].value("model", "");
                    if (!gmModel.empty()) {
                        std::string mapped = mapModelPlaceholderToName(gmModel);
                        if (!mapped.empty() && (mapped != "gemini-3.8-flash-tiered" || fallbackM322Model.empty())) {
                            return mapped;
                        }
                    }
                }
            }
        }
        if (!fallbackM322Model.empty()) {
            return fallbackM322Model;
        }
        return "gemini-4-argon-medium";
    };

    auto resolveCascadeConfigForRpc = [](const LsEndpoint& ep,
                                         const std::string& targetCid,
                                         const std::string& requestedModel,
                                         const std::string& workspacePath) -> nlohmann::json {
        auto extractBestCascadeConfigFromTrajectory = [](const nlohmann::json& tj, nlohmann::json& outConfig) -> bool {
            if (!tj.is_object() || !tj.contains("trajectory") || !tj["trajectory"].is_object()) {
                return false;
            }
            const auto& traj = tj["trajectory"];
            if (!traj.contains("executorMetadatas") || !traj["executorMetadatas"].is_array()) {
                return false;
            }
            const auto& ems = traj["executorMetadatas"];
            nlohmann::json latestCascadeConfig = nlohmann::json::object();
            nlohmann::json richCascadeConfig = nlohmann::json::object();
            nlohmann::json recoveredDynamicUris = nlohmann::json::array();

            for (auto it = ems.rbegin(); it != ems.rend(); ++it) {
                if (!it->is_object() || !it->contains("cascadeConfig") || !(*it)["cascadeConfig"].is_object()) {
                    continue;
                }
                const auto& cc = (*it)["cascadeConfig"];
                if (cc.empty()) {
                    continue;
                }
                if (latestCascadeConfig.empty()) {
                    latestCascadeConfig = cc;
                }
                if (recoveredDynamicUris.empty() && cc.contains("dynamicWorkspaceUris") &&
                    cc["dynamicWorkspaceUris"].is_array() && !cc["dynamicWorkspaceUris"].empty()) {
                    recoveredDynamicUris = cc["dynamicWorkspaceUris"];
                }
                if (richCascadeConfig.empty() && cc.contains("plannerConfig") && cc["plannerConfig"].is_object()) {
                    const auto& pc = cc["plannerConfig"];
                    if (pc.contains("customizationConfig") && pc["customizationConfig"].is_object() &&
                        !pc["customizationConfig"].empty()) {
                        richCascadeConfig = cc;
                    }
                }
                if (!latestCascadeConfig.empty() && !richCascadeConfig.empty() && !recoveredDynamicUris.empty()) {
                    break;
                }
            }

            if (latestCascadeConfig.empty() && richCascadeConfig.empty()) {
                return false;
            }

            outConfig = !richCascadeConfig.empty() ? richCascadeConfig : latestCascadeConfig;
            if (!recoveredDynamicUris.empty() &&
                (!outConfig.contains("dynamicWorkspaceUris") || !outConfig["dynamicWorkspaceUris"].is_array() ||
                 outConfig["dynamicWorkspaceUris"].empty())) {
                outConfig["dynamicWorkspaceUris"] = recoveredDynamicUris;
            }

            if (!outConfig.contains("plannerConfig") || !outConfig["plannerConfig"].is_object()) {
                outConfig["plannerConfig"] = nlohmann::json::object();
            }
            auto& pc = outConfig["plannerConfig"];
            bool latestHadCustom = latestCascadeConfig.contains("plannerConfig") &&
                                   latestCascadeConfig["plannerConfig"].is_object() &&
                                   latestCascadeConfig["plannerConfig"].contains("customizationConfig") &&
                                   latestCascadeConfig["plannerConfig"]["customizationConfig"].is_object() &&
                                   !latestCascadeConfig["plannerConfig"]["customizationConfig"].empty();
            std::string curPlanModel = pc.value("planModel", "");
            if (curPlanModel.empty() || (curPlanModel == "MODEL_PLACEHOLDER_M322" && !latestHadCustom)) {
                pc["planModel"] = "MODEL_PLACEHOLDER_M418";
                if (pc.contains("modelName") && pc["modelName"].is_string()) {
                    pc["modelName"] = "gemini-4-argon-medium";
                }
            }
            return true;
        };

        nlohmann::json cascadeConfig = nlohmann::json::object();
        bool resolved = false;

        if (ep.port > 0 && ep.port <= 65535 && !ep.csrf.empty()) {
            try {
                httplib::Client cli("127.0.0.1", ep.port);
                cli.set_connection_timeout(0, 500000);
                cli.set_read_timeout(2, 0);
                httplib::Headers hdrs = {
                    {"Content-Type", "application/json"},
                    {"x-codeium-csrf-token", ep.csrf}
                };
                if (!targetCid.empty() && targetCid != "new") {
                    auto trajRes = cli.Post(
                        "/exa.language_server_pb.LanguageServerService/GetCascadeTrajectory",
                        hdrs,
                        nlohmann::json({{"cascadeId", targetCid}}).dump(),
                        "application/json"
                    );
                    if (trajRes && trajRes->status == 200) {
                        auto tj = nlohmann::json::parse(trajRes->body, nullptr, false);
                        if (!tj.is_discarded()) {
                            resolved = extractBestCascadeConfigFromTrajectory(tj, cascadeConfig);
                        }
                    }
                }
                if (!resolved) {
                    auto allRes = cli.Post(
                        "/exa.language_server_pb.LanguageServerService/GetAllCascadeTrajectories",
                        hdrs,
                        "{}",
                        "application/json"
                    );
                    if (allRes && allRes->status == 200) {
                        auto aj = nlohmann::json::parse(allRes->body, nullptr, false);
                        if (!aj.is_discarded() && aj.is_object() &&
                            aj.contains("trajectorySummaries") && aj["trajectorySummaries"].is_object()) {
                            int probed = 0;
                            for (auto it = aj["trajectorySummaries"].begin();
                                 it != aj["trajectorySummaries"].end() && probed < 5; ++it) {
                                std::string otherCid = it.key();
                                if (otherCid.empty() || otherCid == targetCid) continue;
                                ++probed;
                                auto otherRes = cli.Post(
                                    "/exa.language_server_pb.LanguageServerService/GetCascadeTrajectory",
                                    hdrs,
                                    nlohmann::json({{"cascadeId", otherCid}}).dump(),
                                    "application/json"
                                );
                                if (otherRes && otherRes->status == 200) {
                                    auto otj = nlohmann::json::parse(otherRes->body, nullptr, false);
                                    if (!otj.is_discarded() &&
                                        extractBestCascadeConfigFromTrajectory(otj, cascadeConfig)) {
                                        resolved = true;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            } catch (...) {}
        }

        if (!cascadeConfig.is_object()) {
            cascadeConfig = nlohmann::json::object();
        }
        if (!cascadeConfig.contains("plannerConfig") || !cascadeConfig["plannerConfig"].is_object()) {
            cascadeConfig["plannerConfig"] = nlohmann::json::object();
        }
        auto& pc = cascadeConfig["plannerConfig"];
        if (!pc.contains("planModel") || !pc["planModel"].is_string() || pc["planModel"].get<std::string>().empty()) {
            pc["planModel"] = "MODEL_PLACEHOLDER_M418";
        }
        if ((!cascadeConfig.contains("dynamicWorkspaceUris") || !cascadeConfig["dynamicWorkspaceUris"].is_array() ||
             cascadeConfig["dynamicWorkspaceUris"].empty()) && !workspacePath.empty()) {
            cascadeConfig["dynamicWorkspaceUris"] = nlohmann::json::array({"file://" + workspacePath});
        }

        if (!requestedModel.empty()) {
            if (requestedModel == "MODEL_PLACEHOLDER_M418" || requestedModel == "gemini-4-argon-medium" ||
                requestedModel == "pro") {
                pc["planModel"] = "MODEL_PLACEHOLDER_M418";
                if (pc.contains("modelName")) {
                    pc["modelName"] = "gemini-4-argon-medium";
                }
            } else if (requestedModel == "MODEL_PLACEHOLDER_M322" || requestedModel == "gemini-3.8-flash-tiered" ||
                       requestedModel == "flash" || requestedModel == "flash_lite") {
                pc["planModel"] = "MODEL_PLACEHOLDER_M322";
                if (pc.contains("modelName")) {
                    pc["modelName"] = "gemini-3.8-flash-tiered";
                }
            } else if (requestedModel.rfind("MODEL_", 0) == 0) {
                pc["planModel"] = requestedModel;
            }
        }

        return cascadeConfig;
    };

    _server->Get("/api/agent/transcript", [getIdeBrainDir, getCliBrainDir, stripUserRequestTags, discoverLsEndpoints, lastRpcEndpoint, lastRpcMutex, extractTrajectoryModelName](
                                              const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        std::string id = req.get_param_value("id");
        if (id.empty() || id.find("..") != std::string::npos || id.find('/') != std::string::npos) {
            res.status = 400;
            res.set_content("{\"error\":\"Missing or invalid conversation id\"}", "application/json");
            return;
        }

        int sinceIndex = -1;
        if (req.has_param("since")) {
            try {
                sinceIndex = std::stoi(req.get_param_value("since"));
            } catch (...) {
                sinceIndex = -1;
            }
        }

        fs::path idePath = fs::path(getIdeBrainDir()) / id / ".system_generated" / "logs" / "transcript.jsonl";
        fs::path cliPath = fs::path(getCliBrainDir()) / id / ".system_generated" / "logs" / "transcript.jsonl";
        fs::path transcriptPath;
        std::string source = "ide";

        if (fs::exists(idePath) && fs::exists(cliPath)) {
            try {
                if (fs::last_write_time(cliPath) >= fs::last_write_time(idePath)) {
                    transcriptPath = cliPath;
                    source = "cli";
                } else {
                    transcriptPath = idePath;
                    source = "ide";
                }
            } catch (...) {
                transcriptPath = cliPath;
                source = "cli";
            }
        } else if (fs::exists(cliPath)) {
            transcriptPath = cliPath;
            source = "cli";
        } else if (fs::exists(idePath)) {
            transcriptPath = idePath;
            source = "ide";
        } else {
            // Fallback: check live in-memory trajectory via ConnectRPC GetCascadeTrajectory
            nlohmann::json liveSteps = nlohmann::json::array();
            int liveTotalSteps = 0;
            std::string liveModel = "gemini-4-argon-medium";
            bool foundInMemory = false;

            auto tryProbeRpcTrajectory = [&](int port, const std::string& csrf) -> bool {
                if (port <= 0 || port > 65535 || csrf.empty()) return false;
                try {
                    httplib::Client cli("127.0.0.1", port);
                    cli.set_connection_timeout(0, 400000);
                    cli.set_read_timeout(1, 0);
                    httplib::Headers hdrs = {
                        {"Content-Type", "application/json"},
                        {"x-codeium-csrf-token", csrf}
                    };
                    auto rpcRes = cli.Post(
                        "/exa.language_server_pb.LanguageServerService/GetCascadeTrajectory",
                        hdrs,
                        nlohmann::json({{"cascadeId", id}}).dump(),
                        "application/json"
                    );
                    if (rpcRes && rpcRes->status == 200) {
                        auto tj = nlohmann::json::parse(rpcRes->body, nullptr, false);
                        if (!tj.is_discarded() && tj.contains("trajectory") && tj["trajectory"].is_object()) {
                            liveModel = extractTrajectoryModelName(tj);
                            const auto& rawSteps = tj["trajectory"].value("steps", nlohmann::json::array());
                            int idx = 0;
                            for (const auto& rs : rawSteps) {
                                int stepIdx = idx++;
                                liveTotalSteps = std::max(liveTotalSteps, stepIdx + 1);
                                if (sinceIndex >= 0 && stepIdx <= sinceIndex) continue;
                                std::string stType = rs.value("type", "");
                                if (stType == "CORTEX_STEP_TYPE_USER_INPUT" && rs.contains("userInput")) {
                                    std::string uText = rs["userInput"].value("userResponse", "");
                                    if (uText.empty() && rs["userInput"].contains("items") && rs["userInput"]["items"].is_array() && !rs["userInput"]["items"].empty()) {
                                        uText = rs["userInput"]["items"][0].value("text", "");
                                    }
                                    liveSteps.push_back({
                                        {"index", stepIdx},
                                        {"stepIndex", stepIdx},
                                        {"type", "USER_INPUT"},
                                        {"text", uText},
                                        {"userInput", uText},
                                        {"step", {{"case", "userInput"}, {"value", uText}}}
                                    });
                                } else if (stType == "CORTEX_STEP_TYPE_PLANNER_RESPONSE" && rs.contains("plannerResponse")) {
                                    std::string pText = rs["plannerResponse"].value("modifiedResponse", rs["plannerResponse"].value("response", ""));
                                    std::string pThink = rs["plannerResponse"].value("thinking", "");
                                    liveSteps.push_back({
                                        {"index", stepIdx},
                                        {"stepIndex", stepIdx},
                                        {"type", "PLANNER_RESPONSE"},
                                        {"thinking", pThink},
                                        {"plannerResponse", pThink},
                                        {"text", pText},
                                        {"tool_calls", nlohmann::json::array()},
                                        {"step", {{"case", !pText.empty() ? "text" : "plannerResponse"}, {"value", !pText.empty() ? pText : pThink}}}
                                    });
                                } else if (stType == "CORTEX_STEP_TYPE_ERROR_MESSAGE" && rs.contains("errorMessage")) {
                                    std::string errText = "Agent execution error";
                                    if (rs["errorMessage"].contains("error") && rs["errorMessage"]["error"].is_object()) {
                                        errText = rs["errorMessage"]["error"].value("shortError",
                                                  rs["errorMessage"]["error"].value("userErrorMessage", errText));
                                    }
                                    liveSteps.push_back({
                                        {"index", stepIdx},
                                        {"stepIndex", stepIdx},
                                        {"type", "ERROR_MESSAGE"},
                                        {"text", errText},
                                        {"step", {{"case", "errorMessage"}, {"value", errText}}}
                                    });
                                }
                            }
                            return true;
                        }
                    }
                } catch (...) {}
                return false;
            };

            const char* envLsPort = std::getenv("AGY_LS_PORT");
            const char* envLsCsrf = std::getenv("AGY_LS_CSRF_TOKEN");
            if (envLsPort && *envLsPort && envLsCsrf && *envLsCsrf) {
                try {
                    if (tryProbeRpcTrajectory(std::stoi(envLsPort), envLsCsrf)) {
                        foundInMemory = true;
                    }
                } catch (...) {}
            }
            if (!foundInMemory) {
                int cachedPort = 0;
                std::string cachedCsrf;
                {
                    std::lock_guard<std::mutex> lk(*lastRpcMutex);
                    cachedPort = lastRpcEndpoint->first;
                    cachedCsrf = lastRpcEndpoint->second;
                }
                if (cachedPort > 0 && !cachedCsrf.empty()) {
                    if (tryProbeRpcTrajectory(cachedPort, cachedCsrf)) {
                        foundInMemory = true;
                    }
                }
            }
            if (!foundInMemory) {
                for (const auto& ep : discoverLsEndpoints()) {
                    if (tryProbeRpcTrajectory(ep.port, ep.csrf)) {
                        std::lock_guard<std::mutex> lk(*lastRpcMutex);
                        *lastRpcEndpoint = {ep.port, ep.csrf};
                        foundInMemory = true;
                        break;
                    }
                }
            }

            if (foundInMemory) {
                nlohmann::json resp;
                resp["conversationId"] = id;
                resp["source"] = "live";
                resp["model"] = liveModel;
                resp["stepCount"] = liveTotalSteps;
                resp["steps"] = liveSteps;
                res.set_content(resp.dump(2), "application/json");
                return;
            }

            res.status = 404;
            res.set_content("{\"error\":\"Conversation transcript not found\"}", "application/json");
            return;
        }

        std::ifstream f(transcriptPath);
        std::string line;
        nlohmann::json steps = nlohmann::json::array();
        int lineNum = 0;
        int totalStepCount = 0;

        while (std::getline(f, line)) {
            if (line.empty()) continue;
            try {
                auto j = nlohmann::json::parse(line, nullptr, false);
                if (j.is_discarded() || !j.is_object()) continue;

                int stepIdx = j.value("step_index", lineNum);
                ++lineNum;
                totalStepCount = std::max(totalStepCount, stepIdx + 1);

                if (sinceIndex >= 0 && stepIdx <= sinceIndex) {
                    continue;
                }

                std::string type = j.value("type", "");
                std::string content = j.value("content", "");
                std::string thinking = j.value("thinking", "");
                std::string createdAt = j.value("created_at", "");

                nlohmann::json parsedToolCalls = nlohmann::json::array();
                if (j.contains("tool_calls") && j["tool_calls"].is_array()) {
                    for (const auto& tc : j["tool_calls"]) {
                        if (!tc.is_object()) continue;
                        std::string toolName = tc.value("name", "tool");
                        nlohmann::json argsObj = nlohmann::json::object();
                        if (tc.contains("arguments") && tc["arguments"].is_object()) {
                            argsObj = tc["arguments"];
                        } else if (tc.contains("args") && tc["args"].is_object()) {
                            argsObj = tc["args"];
                        }

                        auto unquoteStr = [](const std::string& s) -> std::string {
                            if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
                                return s.substr(1, s.size() - 2);
                            }
                            return s;
                        };

                        std::string toolAction = tc.value("toolAction", "");
                        if (toolAction.empty() && argsObj.contains("toolAction") && argsObj["toolAction"].is_string()) {
                            toolAction = unquoteStr(argsObj["toolAction"].get<std::string>());
                        }
                        std::string toolSummary = tc.value("toolSummary", "");
                        if (toolSummary.empty() && argsObj.contains("toolSummary") && argsObj["toolSummary"].is_string()) {
                            toolSummary = unquoteStr(argsObj["toolSummary"].get<std::string>());
                        }
                        std::string cmd;
                        if (argsObj.contains("CommandLine") && argsObj["CommandLine"].is_string()) {
                            cmd = unquoteStr(argsObj["CommandLine"].get<std::string>());
                        }

                        parsedToolCalls.push_back({
                            {"name", toolName},
                            {"toolAction", toolAction},
                            {"toolSummary", toolSummary},
                            {"command", cmd},
                            {"arguments", argsObj}
                        });
                    }
                }

                if (type == "USER_INPUT") {
                    std::string clean = stripUserRequestTags(content);
                    steps.push_back({
                        {"index", stepIdx},
                        {"stepIndex", stepIdx},
                        {"type", "USER_INPUT"},
                        {"text", clean},
                        {"userInput", clean},
                        {"createdAt", createdAt},
                        {"step", {
                            {"case", "userInput"},
                            {"value", clean}
                        }}
                    });
                } else if (type == "PLANNER_RESPONSE") {
                    std::string displayValue = !content.empty() ? content : thinking;
                    steps.push_back({
                        {"index", stepIdx},
                        {"stepIndex", stepIdx},
                        {"type", "PLANNER_RESPONSE"},
                        {"thinking", thinking},
                        {"plannerResponse", thinking},
                        {"text", content},
                        {"tool_calls", parsedToolCalls},
                        {"createdAt", createdAt},
                        {"step", {
                            {"case", !content.empty() ? "text" : "plannerResponse"},
                            {"value", displayValue}
                        }}
                    });
                } else if (type == "ERROR_MESSAGE") {
                    std::string errText = !content.empty() ? content : j.value("error", "Agent execution error");
                    steps.push_back({
                        {"index", stepIdx},
                        {"stepIndex", stepIdx},
                        {"type", "ERROR_MESSAGE"},
                        {"text", errText},
                        {"createdAt", createdAt},
                        {"step", {
                            {"case", "errorMessage"},
                            {"value", errText}
                        }}
                    });
                } else if (!parsedToolCalls.empty()) {
                    std::string firstTool = parsedToolCalls[0].value("name", "tool");
                    std::string firstCmd = parsedToolCalls[0].value("command", "");
                    steps.push_back({
                        {"index", stepIdx},
                        {"stepIndex", stepIdx},
                        {"type", type.empty() ? "TOOL_CALL" : type},
                        {"thinking", thinking},
                        {"text", content},
                        {"tool_calls", parsedToolCalls},
                        {"createdAt", createdAt},
                        {"step", {
                            {"case", "toolCall"},
                            {"tool", firstTool},
                            {"command", firstCmd}
                        }}
                    });
                }
            } catch (...) {}
        }

        nlohmann::json resp;
        resp["conversationId"] = id;
        resp["source"] = source;
        resp["model"] = "gemini-4-argon-medium";
        resp["stepCount"] = totalStepCount;
        resp["steps"] = steps;
        res.set_content(resp.dump(2), "application/json");
    });

    _server->Post("/api/agent/prompt", [getWorkspaceRoot, discoverLsEndpoints, lastRpcEndpoint, lastRpcMutex, resolveCascadeConfigForRpc](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        nlohmann::json j;
        try {
            j = nlohmann::json::parse(req.body);
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(nlohmann::json({{"status", "error"}, {"error", e.what()}}).dump(), "application/json");
            return;
        }

        std::string text = j.value("text", j.value("prompt", ""));
        std::string agentId = j.value("agentId", "antigravity");
        std::string cid = j.value("conversationId", "");
        std::string requestedModel = j.value("model", "");
        if (text.empty()) {
            res.status = 400;
            res.set_content("{\"status\":\"error\",\"error\":\"Prompt text cannot be empty\"}", "application/json");
            return;
        }

        std::string agyBin = PathUtils::expandHome("~/.local/bin/agy");
        const char* envAgyBin = std::getenv("AIMON_AGY_BIN");
        if (envAgyBin && *envAgyBin) {
            agyBin = envAgyBin;
        }
        if (!fs::exists(agyBin) || access(agyBin.c_str(), X_OK) != 0) {
            res.status = 503;
            res.set_content(nlohmann::json({
                {"status", "error"},
                {"error", "Antigravity CLI binary (agy) is not available or not executable at " + agyBin}
            }).dump(2), "application/json");
            return;
        }

        std::vector<LsEndpoint> endpoints = discoverLsEndpoints();

        if (endpoints.empty()) {
            res.status = 503;
            res.set_content(nlohmann::json({
                {"status", "error"},
                {"error", "No active Antigravity language server or agy agent session found"}
            }).dump(2), "application/json");
            return;
        }

        std::string wsPath = getWorkspaceRoot().string();

        // 3. Attempt direct ConnectRPC dispatch with conversation ownership verification
        auto sendRpcCascadeMessage = [&](const LsEndpoint& ep, const std::string& targetCid, std::string& outBody) -> bool {
            try {
                nlohmann::json cascadeCfg = resolveCascadeConfigForRpc(ep, targetCid, requestedModel, wsPath);
                httplib::Client cli("127.0.0.1", ep.port);
                cli.set_connection_timeout(1, 0);
                cli.set_read_timeout(5, 0);
                httplib::Headers hdrs = {
                    {"Content-Type", "application/json"},
                    {"x-codeium-csrf-token", ep.csrf}
                };
                nlohmann::json payload = {
                    {"cascadeId", targetCid},
                    {"items", nlohmann::json::array({{{"text", text}}})},
                    {"cascadeConfig", cascadeCfg}
                };
                auto rpcRes = cli.Post(
                    "/exa.language_server_pb.LanguageServerService/SendUserCascadeMessage",
                    hdrs,
                    payload.dump(),
                    "application/json"
                );
                if (rpcRes && rpcRes->status == 200) {
                    outBody = rpcRes->body;
                    std::lock_guard<std::mutex> lk(*lastRpcMutex);
                    *lastRpcEndpoint = {ep.port, ep.csrf};
                    return true;
                }
            } catch (...) {}
            return false;
        };

        auto startRpcNewCascade = [&](const LsEndpoint& ep, std::string& outNewCid) -> bool {
            try {
                httplib::Client cli("127.0.0.1", ep.port);
                cli.set_connection_timeout(1, 0);
                cli.set_read_timeout(5, 0);
                httplib::Headers hdrs = {
                    {"Content-Type", "application/json"},
                    {"x-codeium-csrf-token", ep.csrf}
                };
                nlohmann::json payload = {
                    {"source", "CORTEX_TRAJECTORY_SOURCE_CLI"}
                };
                auto rpcRes = cli.Post(
                    "/exa.language_server_pb.LanguageServerService/StartCascade",
                    hdrs,
                    payload.dump(),
                    "application/json"
                );
                if (rpcRes && rpcRes->status == 200) {
                    auto sj = nlohmann::json::parse(rpcRes->body, nullptr, false);
                    if (!sj.is_discarded() && sj.is_object() && sj.contains("cascadeId") && sj["cascadeId"].is_string()) {
                        outNewCid = sj["cascadeId"].get<std::string>();
                        return !outNewCid.empty();
                    }
                }
            } catch (...) {}
            return false;
        };

        auto endpointOwnsConversation = [&](const LsEndpoint& ep, const std::string& targetCid, bool& outEndpointReachable) -> bool {
            outEndpointReachable = false;
            try {
                httplib::Client cli("127.0.0.1", ep.port);
                cli.set_connection_timeout(0, 500000);
                cli.set_read_timeout(2, 0);
                httplib::Headers hdrs = {
                    {"Content-Type", "application/json"},
                    {"x-codeium-csrf-token", ep.csrf}
                };
                auto metaRes = cli.Post(
                    "/exa.language_server_pb.LanguageServerService/GetConversationMetadata",
                    hdrs,
                    nlohmann::json({{"conversationId", targetCid}}).dump(),
                    "application/json"
                );
                if (metaRes && metaRes->status == 200) {
                    outEndpointReachable = true;
                    auto mj = nlohmann::json::parse(metaRes->body, nullptr, false);
                    if (!mj.is_discarded() && mj.is_object() && mj.contains("metadata")) {
                        return true;
                    }
                }
                auto allRes = cli.Post(
                    "/exa.language_server_pb.LanguageServerService/GetAllCascadeTrajectories",
                    hdrs,
                    "{}",
                    "application/json"
                );
                if (allRes && allRes->status == 200) {
                    outEndpointReachable = true;
                    auto aj = nlohmann::json::parse(allRes->body, nullptr, false);
                    if (!aj.is_discarded() && aj.is_object() &&
                        aj.contains("trajectorySummaries") && aj["trajectorySummaries"].is_object() &&
                        aj["trajectorySummaries"].contains(targetCid)) {
                        return true;
                    }
                }
            } catch (...) {}
            return false;
        };

        if (!cid.empty() && cid != "new") {
            const LsEndpoint* firstReachableEp = nullptr;
            for (const auto& ep : endpoints) {
                bool reachable = false;
                if (endpointOwnsConversation(ep, cid, reachable)) {
                    std::string rpcOut;
                    if (sendRpcCascadeMessage(ep, cid, rpcOut)) {
                        nlohmann::json resp;
                        resp["status"] = "dispatched";
                        resp["agentId"] = agentId;
                        resp["conversationId"] = cid;
                        resp["output"] = rpcOut;
                        res.set_content(resp.dump(2), "application/json");
                        return;
                    }
                }
                if (reachable && !firstReachableEp) {
                    firstReachableEp = &ep;
                }
            }
            // If cid was an archived conversation not loaded in any live server, start a new cascade on the primary reachable server
            if (firstReachableEp != nullptr) {
                std::string newCid;
                std::string rpcOut;
                if (startRpcNewCascade(*firstReachableEp, newCid) &&
                    sendRpcCascadeMessage(*firstReachableEp, newCid, rpcOut)) {
                    nlohmann::json resp;
                    resp["status"] = "dispatched";
                    resp["agentId"] = agentId;
                    resp["conversationId"] = newCid;
                    resp["output"] = rpcOut;
                    res.set_content(resp.dump(2), "application/json");
                    return;
                }
            }
        } else {
            for (const auto& ep : endpoints) {
                std::string newCid;
                std::string rpcOut;
                if (startRpcNewCascade(ep, newCid) && sendRpcCascadeMessage(ep, newCid, rpcOut)) {
                    nlohmann::json resp;
                    resp["status"] = "dispatched";
                    resp["agentId"] = agentId;
                    resp["conversationId"] = newCid;
                    resp["output"] = rpcOut;
                    res.set_content(resp.dump(2), "application/json");
                    return;
                }
            }
        }

        // 4. Fallback: invoke agy agentapi CLI binary
        auto runAgyCommand = [&](const LsEndpoint& ep, int& outExitCode, std::string& outOutput) -> bool {
            int pipeFd[2];
            if (pipe(pipeFd) != 0) {
                return false;
            }
            pid_t child = fork();
            if (child < 0) {
                close(pipeFd[0]);
                close(pipeFd[1]);
                return false;
            }
            if (child == 0) {
                close(pipeFd[0]);
                dup2(pipeFd[1], STDOUT_FILENO);
                dup2(pipeFd[1], STDERR_FILENO);
                close(pipeFd[1]);

                std::string portStr = std::to_string(ep.port);
                std::string lsAddr = "localhost:" + portStr;
                unsetenv("ANTIGRAVITY_CONVERSATION_ID");
                setenv("AGY_LS_PORT", portStr.c_str(), 1);
                setenv("AGY_LS_CSRF_TOKEN", ep.csrf.c_str(), 1);
                setenv("ANTIGRAVITY_LS_ADDRESS", lsAddr.c_str(), 1);
                setenv("ANTIGRAVITY_CSRF_TOKEN", ep.csrf.c_str(), 1);
                setenv("ANTIGRAVITY_PROJECT_ID", "default-cli-project", 1);

                std::string cliModelFlag = "--model=pro";
                if (requestedModel == "flash" || requestedModel == "MODEL_PLACEHOLDER_M322" ||
                    requestedModel == "gemini-3.8-flash-tiered") {
                    cliModelFlag = "--model=flash";
                } else if (requestedModel == "flash_lite") {
                    cliModelFlag = "--model=flash_lite";
                }

                std::vector<char*> argv;
                argv.push_back(const_cast<char*>(agyBin.c_str()));
                argv.push_back(const_cast<char*>("agentapi"));
                if (cid.empty() || cid == "new") {
                    argv.push_back(const_cast<char*>("new-conversation"));
                    argv.push_back(const_cast<char*>(cliModelFlag.c_str()));
                    argv.push_back(const_cast<char*>(text.c_str()));
                } else {
                    argv.push_back(const_cast<char*>("send-message"));
                    argv.push_back(const_cast<char*>(cid.c_str()));
                    argv.push_back(const_cast<char*>(text.c_str()));
                }
                argv.push_back(nullptr);

                execv(agyBin.c_str(), argv.data());
                _exit(127);
            }

            close(pipeFd[1]);
            outOutput.clear();
            char buf[1024];
            ssize_t n;
            while ((n = read(pipeFd[0], buf, sizeof(buf))) > 0) {
                outOutput.append(buf, static_cast<size_t>(n));
            }
            close(pipeFd[0]);

            int status = 0;
            int waitedMs = 0;
            while (waitedMs < 10000) {
                pid_t w = waitpid(child, &status, WNOHANG);
                if (w == child) {
                    outExitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
                waitedMs += 25;
            }
            kill(child, SIGKILL);
            waitpid(child, &status, 0);
            outExitCode = -1;
            return false;
        };

        std::string lastError = "Failed to dispatch prompt via agy agentapi";
        for (const auto& ep : endpoints) {
            int exitCode = -1;
            std::string output;
            if (!runAgyCommand(ep, exitCode, output)) {
                lastError = "agy agentapi execution timed out or failed to spawn";
                continue;
            }

            nlohmann::json parsedOut = nlohmann::json::parse(output, nullptr, false);
            bool hasJsonError = (!parsedOut.is_discarded() && parsedOut.is_object() &&
                                 parsedOut.contains("error") && !parsedOut["error"].is_null() &&
                                 (!parsedOut["error"].is_string() || !parsedOut["error"].get<std::string>().empty()));

            if (exitCode == 0 && !hasJsonError) {
                std::string resolvedCid = (cid == "new") ? "" : cid;
                if (!parsedOut.is_discarded() && parsedOut.is_object()) {
                    if (parsedOut.contains("conversationId") && parsedOut["conversationId"].is_string()) {
                        resolvedCid = parsedOut["conversationId"].get<std::string>();
                    } else if (parsedOut.contains("response") && parsedOut["response"].is_object()) {
                        const auto& rObj = parsedOut["response"];
                        if (rObj.contains("conversationId") && rObj["conversationId"].is_string()) {
                            resolvedCid = rObj["conversationId"].get<std::string>();
                        } else if (rObj.contains("cascadeId") && rObj["cascadeId"].is_string()) {
                            resolvedCid = rObj["cascadeId"].get<std::string>();
                        } else if (rObj.contains("newConversation") && rObj["newConversation"].is_object()) {
                            const auto& nc = rObj["newConversation"];
                            if (nc.contains("conversationId") && nc["conversationId"].is_string()) {
                                resolvedCid = nc["conversationId"].get<std::string>();
                            }
                        }
                    }
                }

                nlohmann::json resp;
                resp["status"] = "dispatched";
                resp["agentId"] = agentId;
                resp["conversationId"] = resolvedCid;
                resp["output"] = output;
                res.set_content(resp.dump(2), "application/json");
                return;
            }

            if (hasJsonError && parsedOut["error"].is_string()) {
                lastError = parsedOut["error"].get<std::string>();
            } else if (!output.empty()) {
                lastError = output;
            } else {
                lastError = "agy exited with code " + std::to_string(exitCode);
            }
        }

        res.status = 503;
        res.set_content(nlohmann::json({
            {"status", "error"},
            {"error", lastError}
        }).dump(2), "application/json");
    });

    _server->Get("/api/agent/artifacts", [getIdeBrainDir, getCliBrainDir](
                                             const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        std::string id = req.get_param_value("id");
        if (id.empty() || id.find("..") != std::string::npos || id.find('/') != std::string::npos) {
            res.status = 400;
            res.set_content("{\"error\":\"Missing or invalid conversation id\"}", "application/json");
            return;
        }

        nlohmann::json artifacts = nlohmann::json::array();
        auto scanArtifactsDir = [&](const fs::path& dir) {
            if (!fs::exists(dir) || !fs::is_directory(dir)) return;
            std::error_code ec;
            for (const auto& entry : fs::directory_iterator(dir, ec)) {
                if (ec) break;
                if (entry.is_regular_file() && entry.path().extension() == ".md") {
                    artifacts.push_back({
                        {"name", entry.path().filename().string()},
                        {"path", entry.path().string()},
                        {"isDir", false}
                    });
                }
            }
        };
        scanArtifactsDir(fs::path(getIdeBrainDir()) / id);
        scanArtifactsDir(fs::path(getCliBrainDir()) / id);

        nlohmann::json resp;
        resp["artifacts"] = artifacts;
        res.set_content(resp.dump(2), "application/json");
    });

    _server->Get("/api/workspace/tree", [getWorkspaceRoot](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        fs::path root = getWorkspaceRoot();
        std::string relSubPath = req.get_param_value("path");
        if (relSubPath.find("..") != std::string::npos) {
            res.status = 400;
            res.set_content("{\"error\":\"Invalid path traversal\"}", "application/json");
            return;
        }

        fs::path startDir = relSubPath.empty() ? root : (root / relSubPath);
        if (!fs::exists(startDir) || !fs::is_directory(startDir)) {
            res.status = 404;
            res.set_content("{\"error\":\"Directory not found\"}", "application/json");
            return;
        }

        auto isSkippedDir = [](const std::string& name) -> bool {
            if (name.empty() || name[0] == '.') return true;
            return name == "build" || name == "node_modules" ||
                   name == "third_party" || name == "__pycache__";
        };

        std::function<nlohmann::json(const fs::path&, int)> buildTree;
        buildTree = [&](const fs::path& dirPath, int depth) -> nlohmann::json {
            nlohmann::json nodes = nlohmann::json::array();
            if (depth > 6) {
                return nodes;
            }
            std::error_code ec;
            std::vector<fs::directory_entry> entries;
            for (const auto& entry : fs::directory_iterator(dirPath, ec)) {
                if (ec) break;
                entries.push_back(entry);
            }
            std::sort(entries.begin(), entries.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) {
                bool aDir = a.is_directory();
                bool bDir = b.is_directory();
                if (aDir != bDir) return aDir > bDir;
                return a.path().filename().string() < b.path().filename().string();
            });

            for (const auto& entry : entries) {
                std::string fname = entry.path().filename().string();
                if (isSkippedDir(fname)) continue;

                std::error_code typeEc;
                bool isDir = entry.is_directory(typeEc);
                uint64_t fsize = 0;
                if (!isDir && entry.is_regular_file(typeEc)) {
                    std::error_code sizeEc;
                    fsize = static_cast<uint64_t>(entry.file_size(sizeEc));
                }

                nlohmann::json node;
                node["name"] = fname;
                node["path"] = fs::relative(entry.path(), root, typeEc).string();
                node["isDir"] = isDir;
                node["size"] = fsize;
                if (isDir && depth < 6) {
                    node["children"] = buildTree(entry.path(), depth + 1);
                }
                nodes.push_back(std::move(node));
            }
            return nodes;
        };

        try {
            nlohmann::json tree = buildTree(startDir, 1);
            nlohmann::json resp;
            resp["workspace"] = root.string();
            resp["path"] = relSubPath;
            resp["tree"] = tree;
            res.set_content(resp.dump(2), "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(nlohmann::json({{"error", e.what()}}).dump(), "application/json");
        }
    });

    _server->Get("/api/workspace/file", [getWorkspaceRoot](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        std::string relPath = req.get_param_value("path");
        if (relPath.empty() || relPath.find("..") != std::string::npos) {
            res.status = 400;
            res.set_content("{\"error\":\"Invalid path\"}", "application/json");
            return;
        }
        fs::path target = getWorkspaceRoot() / relPath;
        if (!fs::exists(target) || fs::is_directory(target)) {
            res.status = 404;
            res.set_content("{\"error\":\"File not found\"}", "application/json");
            return;
        }
        std::ifstream f(target);
        std::stringstream buf;
        buf << f.rdbuf();
        nlohmann::json resp;
        resp["path"] = relPath;
        resp["content"] = buf.str();
        resp["language"] = target.extension().string();
        res.set_content(resp.dump(2), "application/json");
    });

    _server->Post("/api/workspace/file", [getWorkspaceRoot](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        try {
            auto j = nlohmann::json::parse(req.body);
            std::string relPath = j.value("path", "");
            std::string content = j.value("content", "");
            if (relPath.empty() || relPath.find("..") != std::string::npos) {
                res.status = 400;
                res.set_content("{\"error\":\"Invalid path\"}", "application/json");
                return;
            }
            fs::path target = getWorkspaceRoot() / relPath;
            std::ofstream f(target, std::ios::trunc);
            if (!f.is_open()) {
                res.status = 500;
                res.set_content("{\"error\":\"Failed to open file for writing\"}", "application/json");
                return;
            }
            f << content;
            res.set_content("{\"status\":\"ok\"}", "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(nlohmann::json({{"error", e.what()}}).dump(), "application/json");
        }
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

    // Usage Insights & Workflow-Coaching REST API endpoints
    _server->Get("/api/v1/insights", [this](const httplib::Request& req, httplib::Response& res) {
        if (!_insightEngine) {
            res.set_content(nlohmann::json({{"findings", nlohmann::json::array()}}).dump(), "application/json");
            return;
        }
        std::string plat = req.has_param("platform") ? req.get_param_value("platform") : "";
        int days = 30;
        if (req.has_param("days")) {
            try { days = std::stoi(req.get_param_value("days")); } catch (...) {}
        }
        std::string minSev = req.has_param("min_severity") ? req.get_param_value("min_severity") : "";
        res.set_content(_insightEngine->getInsights(plat, days, minSev).dump(), "application/json");
    });

    _server->Get("/api/v1/insights/report", [this](const httplib::Request& req, httplib::Response& res) {
        if (!_insightEngine) {
            res.set_content(nlohmann::json({{"rows", nlohmann::json::array()}}).dump(), "application/json");
            return;
        }
        std::string plat = req.has_param("platform") ? req.get_param_value("platform") : "";
        int days = 30;
        if (req.has_param("days")) {
            try { days = std::stoi(req.get_param_value("days")); } catch (...) {}
        }
        std::string groupBy = req.has_param("group_by") ? req.get_param_value("group_by") : "project";
        res.set_content(_insightEngine->getUsageReport(plat, days, groupBy).dump(), "application/json");
    });

    _server->Get("/api/v1/insights/digest", [this](const httplib::Request& req, httplib::Response& res) {
        int days = 7;
        if (req.has_param("days")) {
            try { days = std::stoi(req.get_param_value("days")); } catch (...) {}
        }
        std::string md = _insightEngine ? _insightEngine->generateWeeklyDigest(days) : "";
        res.set_content(nlohmann::json({{"days", days}, {"markdown", md}}).dump(), "application/json");
    });

    _server->Get("/api/v1/insights/compare", [this](const httplib::Request& req, httplib::Response& res) {
        std::string det = req.has_param("detector") ? req.get_param_value("detector") : "cache_hit_collapse";
        std::string plat = req.has_param("platform") ? req.get_param_value("platform") : "";
        int64_t splitEpoch = 0;
        if (req.has_param("split_epoch")) {
            try { splitEpoch = std::stoll(req.get_param_value("split_epoch")); } catch (...) {}
        }
        if (!_insightEngine) {
            res.set_content(nlohmann::json({{"detector", det}, {"verdict", "insufficient_data"}}).dump(), "application/json");
            return;
        }
        res.set_content(_insightEngine->compareExperiment(det, plat, splitEpoch).dump(), "application/json");
    });

    _server->Post("/api/v1/insights/export", [this](const httplib::Request& req, httplib::Response& res) {
        if (!_insightEngine) {
            res.set_content(nlohmann::json({{"files", nlohmann::json::array()}}).dump(), "application/json");
            return;
        }
        nlohmann::json body = nlohmann::json::parse(req.body, nullptr, false);
        std::string outDir;
        std::string fmt = "jsonl";
        int days = 90;
        bool anon = false;
        if (body.is_object()) {
            outDir = body.value("output_dir", "");
            fmt = body.value("format", "jsonl");
            days = body.value("days", 90);
            anon = body.value("anonymize_projects", false);
        }
        res.set_content(_insightEngine->exportDataset(outDir, fmt, days, anon).dump(), "application/json");
    });

    _server->Post("/api/v1/insights/rebuild", [this](const httplib::Request& req, httplib::Response& res) {
        if (!_insightEngine) {
            res.set_content(nlohmann::json({{"ok", false}}).dump(), "application/json");
            return;
        }
        nlohmann::json body = nlohmann::json::parse(req.body, nullptr, false);
        bool full = body.is_object() ? body.value("full", false) : false;
        std::string histPath = PathUtils::getAimonConfigDir() + "/history.db";
        std::string telPath = PathUtils::getAimonConfigDir() + "/telemetry.db";
        bool ok = _insightEngine->rebuildFromConfig(histPath, telPath, full);
        res.set_content(nlohmann::json({{"ok", ok}}).dump(), "application/json");
    });

    // --- Windows EDA/CAD & Local CPU Accelerator Bridge REST API Endpoints ---

    _server->Get("/api/eda/status", [this](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
        res.set_header("Pragma", "no-cache");
        res.set_header("Expires", "0");

        EdaBridgeStatus st = _edaBridge ? _edaBridge->getStatus() : _stateStore.getStatus().eda;
        if (!st.configSnapshot.is_object()) {
            EdaBridgeConfig fallbackCfg = _configManager ? _configManager->getConfig().eda : EdaBridgeConfig();
            st.configSnapshot = fallbackCfg.toJson(true);
            st.authConfigured = !fallbackCfg.authSecret.empty();
        }
        st.configSnapshot["auth_secret"] = st.authConfigured ? "***" : "";
        res.set_content(st.toJson().dump(2), "application/json");
    });

    _server->Post("/api/eda/config", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        if (!isValidMobileOrUiRequest(req)) {
            res.status = 401;
            res.set_content("{\"error\":\"Unauthorized\"}", "application/json");
            return;
        }

        nlohmann::json body = nlohmann::json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object()) {
            res.status = 400;
            res.set_content(nlohmann::json({{"ok", false}, {"error", "Malformed JSON configuration body"}}).dump(2),
                            "application/json");
            return;
        }

        EdaBridgeConfig updatedCfg = _edaBridge
            ? _edaBridge->getConfig()
            : (_configManager ? _configManager->getConfig().eda : EdaBridgeConfig());
        updatedCfg.fromJson(body);

        std::string valErr;
        if (!updatedCfg.validate(valErr)) {
            res.status = 400;
            res.set_content(nlohmann::json({{"ok", false}, {"error", valErr}}).dump(2), "application/json");
            return;
        }

        if (_configManager) {
            _configManager->getConfig().eda = updatedCfg;
            _configManager->save();
            _configManager->save(PathUtils::getDefaultConfigFilePath());
            _configManager->saveLibConfig(PathUtils::getDefaultLibConfigFilePath());
        }

        EdaBridgeStatus st;
        if (_edaBridge) {
            _edaBridge->setConfig(updatedCfg);
            st = _edaBridge->getStatus();
        } else {
            EdaBridge tempBridge(updatedCfg);
            st = tempBridge.getStatus();
        }
        st.configSnapshot = updatedCfg.toJson(true);
        st.configSnapshot["auth_secret"] = st.authConfigured ? "***" : "";

        AggregateStatus agg = _stateStore.getStatus();
        agg.eda = st;
        _stateStore.update(agg);

        if (_mcpServer) {
            _mcpServer->notifyToolsListChanged();
        }

        nlohmann::json out = st.toJson();
        out["ok"] = true;
        res.set_content(out.dump(2), "application/json");
    });

    _server->Post("/api/eda/probe", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        if (!isValidMobileOrUiRequest(req)) {
            res.status = 401;
            res.set_content("{\"error\":\"Unauthorized\"}", "application/json");
            return;
        }

        EdaBridgeStatus st = _edaBridge ? _edaBridge->probeNow() : _stateStore.getStatus().eda;
        if (st.configSnapshot.is_object()) {
            st.configSnapshot["auth_secret"] = st.authConfigured ? "***" : "";
        }

        AggregateStatus agg = _stateStore.getStatus();
        agg.eda = st;
        _stateStore.update(agg);

        nlohmann::json out = st.toJson();
        out["ok"] = st.connected;
        res.set_content(out.dump(2), "application/json");
    });
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

    if (_config.port == 0) {
        int boundPort = _server->bind_to_any_port(_config.host.c_str());
        if (boundPort <= 0) {
            std::cerr << "[WebServer] Failed to bind to any port on " << _config.host << std::endl;
            _running = false;
            return false;
        }
        _config.port = boundPort;
        if (async) {
            _thread = std::make_unique<std::thread>([this]() {
                if (!_server->listen_after_bind()) {
                    _running = false;
                }
            });
            _server->wait_until_ready();
            return _running;
        } else {
            return _server->listen_after_bind();
        }
    }

    if (async) {
        _thread = std::make_unique<std::thread>([this]() {
            if (!_server->listen(_config.host.c_str(), _config.port)) {
                std::cerr << "[WebServer] Failed to bind to "
                          << _config.host << ":" << _config.port << std::endl;
                _running = false;
            }
        });
        _server->wait_until_ready();
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
