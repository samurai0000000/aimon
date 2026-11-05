/*
 * TestMcpStreamableHttp.cxx
 *
 * Qualification suite for MCP Streamable HTTP transport engine,
 * JSON-RPC 2.0 batching, notification semantics (202 Accepted),
 * standalone SSE streaming hygiene, session lifecycle and reaper,
 * and legacy HTTP+SSE backward compatibility.
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "AgentTelemetryDb.hxx"
#include "DynamicToolRegistry.hxx"
#include "HistoryStore.hxx"
#include "McpServer.hxx"
#include "StateStore.hxx"
#include "WebServer.hxx"
#include "Version.hxx"

// CppUTest headers must come last
#include <CppUTest/TestHarness.h>
#include <CppUTest/CommandLineTestRunner.h>
#include <CppUTest/MemoryLeakWarningPlugin.h>

namespace fs = std::filesystem;
using namespace aimon;
using json = nlohmann::json;

namespace {

std::string makeTempDir(const std::string& tag) {
    static std::atomic<uint64_t> s_seq{0};
    fs::path base = fs::temp_directory_path() /
        ("aimon_mcp_" + tag + "_" + std::to_string(::getpid()) + "_" +
         std::to_string(s_seq.fetch_add(1)));
    std::error_code ec;
    fs::create_directories(base, ec);
    return base.string();
}

} // anonymous namespace

/*
 * ----------------------------------------------------------------------------
 * TEST_GROUP(McpStreamableHttp_Protocol)
 * ----------------------------------------------------------------------------
 */
TEST_GROUP(McpStreamableHttp_Protocol) {
    std::string _ws;
    std::unique_ptr<HistoryStore> _historyStore;
    std::unique_ptr<McpServer> _mcpServer;
    std::unique_ptr<WebServer> _webServer;
    int _port = 0;

    void setup() override {
        _ws = makeTempDir("protocol");
        ::setenv("AIMON_CLI_BRAIN_DIR", _ws.c_str(), 1);
        ::setenv("AIMON_IDE_BRAIN_DIR", _ws.c_str(), 1);

        AgentTelemetryDb::getInstance().open(_ws + "/agent_telemetry.db");

        _historyStore = std::make_unique<HistoryStore>();
        _historyStore->open(_ws + "/history.db");

        _mcpServer = std::make_unique<McpServer>(StateStore::getInstance());

        WebConfig config;
        config.host = "127.0.0.1";
        config.port = 0; // ephemeral port
        config.endpointsEnabled = true;

        _webServer = std::make_unique<WebServer>(
            StateStore::getInstance(), *_historyStore, config, nullptr, _mcpServer.get(), nullptr
        );

        _mcpServer->setNotificationBroadcaster([this](const std::string& notif) {
            if (_webServer) {
                _webServer->broadcastSseNotification(notif);
            }
        });

        CHECK_TRUE(_webServer->start(true));
        _port = _webServer->getPort();
        CHECK_TRUE(_port > 0);
    }

    void teardown() override {
        if (_webServer) {
            _webServer->stop();
            _webServer.reset();
        }
        _mcpServer.reset();
        if (_historyStore) {
            _historyStore->close();
            _historyStore.reset();
        }
        AgentTelemetryDb::getInstance().close();
        std::error_code ec;
        fs::remove_all(_ws, ec);
    }

    httplib::Client makeClient() {
        httplib::Client cli("127.0.0.1", _port);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(10);
        return cli;
    }
};

TEST(McpStreamableHttp_Protocol, NominalInitialize) {
    auto cli = makeClient();
    json initReq = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "initialize"},
        {"params", {
            {"protocolVersion", "2025-03-26"},
            {"clientInfo", {
                {"name", "streamable-test-client"},
                {"version", "1.0.0"}
            }}
        }}
    };

    auto res = cli.Post("/mcp", initReq.dump(), "application/json");
    CHECK_TRUE(res != nullptr);
    LONGS_EQUAL(200, res->status);
    CHECK_TRUE(res->has_header("Mcp-Session-Id"));
    std::string sessionId = res->get_header_value("Mcp-Session-Id");
    CHECK_FALSE(sessionId.empty());

    json resp = json::parse(res->body, nullptr, false);
    CHECK_FALSE(resp.is_discarded());
    CHECK_EQUAL("2.0", resp.value("jsonrpc", ""));
    LONGS_EQUAL(1, resp.value("id", 0));
    CHECK_EQUAL("2025-03-26", resp["result"].value("protocolVersion", ""));
    CHECK_EQUAL("aimon", resp["result"]["serverInfo"].value("name", ""));
    CHECK_TRUE(resp["result"]["capabilities"]["tools"]["listChanged"].get<bool>());
    CHECK_TRUE(resp["result"]["capabilities"]["resources"].is_object());
    CHECK_TRUE(resp["result"]["capabilities"]["prompts"].is_object());
}

TEST(McpStreamableHttp_Protocol, NotificationInitialized) {
    auto cli = makeClient();
    json notif = {
        {"jsonrpc", "2.0"},
        {"method", "notifications/initialized"},
        {"params", json::object()}
    };

    auto res = cli.Post("/mcp", notif.dump(), "application/json");
    CHECK_TRUE(res != nullptr);
    LONGS_EQUAL(202, res->status);
    CHECK_TRUE(res->body.empty());
}

TEST(McpStreamableHttp_Protocol, BatchRequestsAndNotifications) {
    auto cli = makeClient();

    // 1. Array containing requests and a notification
    json batchReq = json::array({
        {
            {"jsonrpc", "2.0"},
            {"id", 10},
            {"method", "initialize"},
            {"params", {{"protocolVersion", "2025-03-26"}}}
        },
        {
            {"jsonrpc", "2.0"},
            {"id", 11},
            {"method", "ping"}
        },
        {
            {"jsonrpc", "2.0"},
            {"id", 12},
            {"method", "tools/list"}
        },
        {
            {"jsonrpc", "2.0"},
            {"method", "notifications/initialized"}
        }
    });

    auto resBatch = cli.Post("/mcp", batchReq.dump(), "application/json");
    CHECK_TRUE(resBatch != nullptr);
    LONGS_EQUAL(200, resBatch->status);

    json resArr = json::parse(resBatch->body, nullptr, false);
    CHECK_FALSE(resArr.is_discarded());
    CHECK_TRUE(resArr.is_array());
    LONGS_EQUAL(3, resArr.size());
    LONGS_EQUAL(10, resArr[0]["id"].get<int>());
    CHECK_EQUAL("2025-03-26", resArr[0]["result"]["protocolVersion"].get<std::string>());
    LONGS_EQUAL(11, resArr[1]["id"].get<int>());
    LONGS_EQUAL(12, resArr[2]["id"].get<int>());
    CHECK_TRUE(resArr[2]["result"]["tools"].is_array());

    // 2. Batch containing only notifications returns 202 Accepted with empty body
    json notifsOnly = json::array({
        {
            {"jsonrpc", "2.0"},
            {"method", "notifications/initialized"}
        },
        {
            {"jsonrpc", "2.0"},
            {"method", "notifications/initialized"}
        }
    });

    auto resNotifs = cli.Post("/mcp", notifsOnly.dump(), "application/json");
    CHECK_TRUE(resNotifs != nullptr);
    LONGS_EQUAL(202, resNotifs->status);
    CHECK_TRUE(resNotifs->body.empty());

    // 3. Empty batch returns 400 Bad Request with code -32600
    auto resEmpty = cli.Post("/mcp", "[]", "application/json");
    CHECK_TRUE(resEmpty != nullptr);
    LONGS_EQUAL(400, resEmpty->status);
    json errEmpty = json::parse(resEmpty->body, nullptr, false);
    CHECK_FALSE(errEmpty.is_discarded());
    LONGS_EQUAL(-32600, errEmpty["error"]["code"].get<int>());
}

TEST(McpStreamableHttp_Protocol, ToolsAndStandardMethods) {
    auto cli = makeClient();

    // 1. tools/list
    json listReq = {
        {"jsonrpc", "2.0"},
        {"id", 20},
        {"method", "tools/list"}
    };
    auto resList = cli.Post("/mcp", listReq.dump(), "application/json");
    CHECK_TRUE(resList != nullptr);
    LONGS_EQUAL(200, resList->status);
    json listJson = json::parse(resList->body, nullptr, false);
    CHECK_FALSE(listJson.is_discarded());
    CHECK_TRUE(listJson["result"]["tools"].is_array());
    bool foundAntigravityQuota = false;
    for (const auto& tool : listJson["result"]["tools"]) {
        if (tool.value("name", "") == "check_antigravity_quota") {
            foundAntigravityQuota = true;
            break;
        }
    }
    CHECK_TRUE(foundAntigravityQuota);

    // 2. tools/call check_antigravity_quota
    json callReq = {
        {"jsonrpc", "2.0"},
        {"id", 21},
        {"method", "tools/call"},
        {"params", {
            {"name", "check_antigravity_quota"},
            {"arguments", json::object()}
        }}
    };
    auto resCall = cli.Post("/mcp", callReq.dump(), "application/json");
    CHECK_TRUE(resCall != nullptr);
    LONGS_EQUAL(200, resCall->status);
    json callJson = json::parse(resCall->body, nullptr, false);
    CHECK_FALSE(callJson.is_discarded());
    CHECK_TRUE(callJson["result"]["content"].is_array());
    CHECK_FALSE(callJson["result"]["content"].empty());
    CHECK_EQUAL("text", callJson["result"]["content"][0].value("type", ""));

    // 3. resources/list
    json resReq = {
        {"jsonrpc", "2.0"},
        {"id", 22},
        {"method", "resources/list"}
    };
    auto resRes = cli.Post("/mcp", resReq.dump(), "application/json");
    CHECK_TRUE(resRes != nullptr);
    LONGS_EQUAL(200, resRes->status);
    json resJson = json::parse(resRes->body, nullptr, false);
    CHECK_FALSE(resJson.is_discarded());
    CHECK_TRUE(resJson["result"]["resources"].is_array());

    // 4. prompts/list
    json promptReq = {
        {"jsonrpc", "2.0"},
        {"id", 23},
        {"method", "prompts/list"}
    };
    auto resPrompt = cli.Post("/mcp", promptReq.dump(), "application/json");
    CHECK_TRUE(resPrompt != nullptr);
    LONGS_EQUAL(200, resPrompt->status);
    json promptJson = json::parse(resPrompt->body, nullptr, false);
    CHECK_FALSE(promptJson.is_discarded());
    CHECK_TRUE(promptJson["result"]["prompts"].is_array());
}

TEST(McpStreamableHttp_Protocol, FaultInjection) {
    auto cli = makeClient();

    // 1. Malformed JSON
    auto res1 = cli.Post("/mcp", "{\"bad\": json syntax unclosed", "application/json");
    CHECK_TRUE(res1 != nullptr);
    LONGS_EQUAL(400, res1->status);
    json err1 = json::parse(res1->body, nullptr, false);
    CHECK_FALSE(err1.is_discarded());
    LONGS_EQUAL(-32700, err1["error"]["code"].get<int>());

    // 2. Non-object primitive payload
    auto res2 = cli.Post("/mcp", "123", "application/json");
    CHECK_TRUE(res2 != nullptr);
    LONGS_EQUAL(400, res2->status);
    json err2 = json::parse(res2->body, nullptr, false);
    CHECK_FALSE(err2.is_discarded());
    LONGS_EQUAL(-32600, err2["error"]["code"].get<int>());

    // 3. Unknown method
    json unkReq = {
        {"jsonrpc", "2.0"},
        {"id", 30},
        {"method", "nonexistent_mcp_function_xyz"}
    };
    auto res3 = cli.Post("/mcp", unkReq.dump(), "application/json");
    CHECK_TRUE(res3 != nullptr);
    LONGS_EQUAL(200, res3->status);
    json err3 = json::parse(res3->body, nullptr, false);
    CHECK_FALSE(err3.is_discarded());
    LONGS_EQUAL(-32601, err3["error"]["code"].get<int>());
}

TEST(McpStreamableHttp_Protocol, PostSseAutodetection) {
    auto cli = makeClient();

    // 1. POST /sse with initialize
    json initReq = {
        {"jsonrpc", "2.0"},
        {"id", 40},
        {"method", "initialize"},
        {"params", {{"protocolVersion", "2025-03-26"}}}
    };
    auto res1 = cli.Post("/sse", initReq.dump(), "application/json");
    CHECK_TRUE(res1 != nullptr);
    LONGS_EQUAL(200, res1->status);
    CHECK_TRUE(res1->has_header("Mcp-Session-Id"));
    json initJson = json::parse(res1->body, nullptr, false);
    CHECK_FALSE(initJson.is_discarded());
    CHECK_EQUAL("2025-03-26", initJson["result"].value("protocolVersion", ""));

    // 2. POST /sse with notification
    json notif = {
        {"jsonrpc", "2.0"},
        {"method", "notifications/initialized"}
    };
    auto res2 = cli.Post("/sse", notif.dump(), "application/json");
    CHECK_TRUE(res2 != nullptr);
    LONGS_EQUAL(202, res2->status);
    CHECK_TRUE(res2->body.empty());
}

TEST(McpStreamableHttp_Protocol, CorsPreflight) {
    auto cli = makeClient();

    auto res = cli.Options("/mcp");
    CHECK_TRUE(res != nullptr);
    LONGS_EQUAL(204, res->status);
    CHECK_TRUE(res->has_header("Access-Control-Allow-Methods"));
    std::string methods = res->get_header_value("Access-Control-Allow-Methods");
    CHECK_TRUE_TEXT(methods.find("DELETE") != std::string::npos, "CORS Allow-Methods must include DELETE");
    CHECK_TRUE(res->has_header("Access-Control-Expose-Headers"));
    std::string expose = res->get_header_value("Access-Control-Expose-Headers");
    CHECK_TRUE_TEXT(expose.find("Mcp-Session-Id") != std::string::npos, "CORS Expose-Headers must include Mcp-Session-Id");
}

/*
 * ----------------------------------------------------------------------------
 * TEST_GROUP(McpStreamableHttp_SseStream)
 * ----------------------------------------------------------------------------
 */
TEST_GROUP(McpStreamableHttp_SseStream) {
    std::string _ws;
    std::unique_ptr<HistoryStore> _historyStore;
    std::unique_ptr<McpServer> _mcpServer;
    std::unique_ptr<WebServer> _webServer;
    int _port = 0;

    void setup() override {
        _ws = makeTempDir("sse_stream");
        ::setenv("AIMON_CLI_BRAIN_DIR", _ws.c_str(), 1);
        ::setenv("AIMON_IDE_BRAIN_DIR", _ws.c_str(), 1);

        AgentTelemetryDb::getInstance().open(_ws + "/agent_telemetry.db");

        _historyStore = std::make_unique<HistoryStore>();
        _historyStore->open(_ws + "/history.db");

        _mcpServer = std::make_unique<McpServer>(StateStore::getInstance());

        WebConfig config;
        config.host = "127.0.0.1";
        config.port = 0;
        config.endpointsEnabled = true;

        _webServer = std::make_unique<WebServer>(
            StateStore::getInstance(), *_historyStore, config, nullptr, _mcpServer.get(), nullptr
        );

        _mcpServer->setNotificationBroadcaster([this](const std::string& notif) {
            if (_webServer) {
                _webServer->broadcastSseNotification(notif);
            }
        });

        CHECK_TRUE(_webServer->start(true));
        _port = _webServer->getPort();
        CHECK_TRUE(_port > 0);
    }

    void teardown() override {
        if (_webServer) {
            _webServer->stop();
            _webServer.reset();
        }
        _mcpServer.reset();
        if (_historyStore) {
            _historyStore->close();
            _historyStore.reset();
        }
        AgentTelemetryDb::getInstance().close();
        std::error_code ec;
        fs::remove_all(_ws, ec);
    }
};

TEST(McpStreamableHttp_SseStream, SseStreamHygieneAndEventIsolation) {
    httplib::Client cli("127.0.0.1", _port);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(10);

    // 1. Establish session via POST /mcp initialize
    json initReq = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "initialize"},
        {"params", {
            {"protocolVersion", "2025-03-26"},
            {"clientInfo", {{"name", "sse-probe"}, {"version", "1.0"}}}
        }}
    };
    auto resInit = cli.Post("/mcp", initReq.dump(), "application/json");
    CHECK_TRUE(resInit != nullptr);
    LONGS_EQUAL(200, resInit->status);
    CHECK_TRUE(resInit->has_header("Mcp-Session-Id"));
    std::string sessionId = resInit->get_header_value("Mcp-Session-Id");
    CHECK_FALSE(sessionId.empty());

    // 2. Open GET /mcp SSE stream in background thread
    std::mutex streamMutex;
    std::condition_variable streamCv;
    std::string receivedData;
    std::atomic<bool> gotKeepalive{false};
    std::atomic<bool> gotToolsChanged{false};
    std::atomic<bool> streamStop{false};

    std::thread sseReader([&]() {
        httplib::Client sseCli("127.0.0.1", _port);
        sseCli.set_read_timeout(10);
        httplib::Headers headers = {
            {"Accept", "text/event-stream"},
            {"Mcp-Session-Id", sessionId}
        };
        sseCli.Get("/mcp", headers, [&](const char* data, size_t len) {
            {
                std::lock_guard<std::mutex> lk(streamMutex);
                receivedData.append(data, len);
                if (receivedData.find(": keepalive") != std::string::npos) {
                    gotKeepalive = true;
                }
                if (receivedData.find("notifications/tools/list_changed") != std::string::npos) {
                    gotToolsChanged = true;
                }
            }
            streamCv.notify_all();
            return !streamStop.load();
        });
    });

    // 3. Wait for initial keepalive comment
    {
        std::unique_lock<std::mutex> lk(streamMutex);
        streamCv.wait_for(lk, std::chrono::seconds(5), [&]() {
            return gotKeepalive.load();
        });
    }
    CHECK_TRUE_TEXT(gotKeepalive.load(), "Initial keepalive comment not received on GET /mcp");

    // 4. Trigger tool list changed notification
    _mcpServer->notifyToolsListChanged();

    // 5. Trigger non-JSON-RPC mobile broadcast (must NOT leak to MCP SSE stream)
    _webServer->broadcastSseNotification("{\"type\":\"mobile_only\",\"battery\":95}");

    // 6. Synchronous POST /mcp ping (response must NOT be duplicated onto SSE stream)
    json pingReq = {
        {"jsonrpc", "2.0"},
        {"id", 77},
        {"method", "ping"}
    };
    httplib::Headers hMcp = {{"Mcp-Session-Id", sessionId}};
    auto resPing = cli.Post("/mcp", hMcp, pingReq.dump(), "application/json");
    CHECK_TRUE(resPing != nullptr);
    LONGS_EQUAL(200, resPing->status);

    // 7. Wait for tool list changed notification to arrive on SSE stream
    {
        std::unique_lock<std::mutex> lk(streamMutex);
        streamCv.wait_for(lk, std::chrono::seconds(5), [&]() {
            return gotToolsChanged.load();
        });
    }
    CHECK_TRUE_TEXT(gotToolsChanged.load(), "notifications/tools/list_changed not received on SSE stream");

    // 8. Clean shutdown of SSE stream
    streamStop = true;
    _webServer->terminateMcpSession(sessionId);
    if (sseReader.joinable()) {
        sseReader.join();
    }

    // 9. Verify stream hygiene constraints
    CHECK_TRUE_TEXT(receivedData.find(": keepalive") != std::string::npos,
                    "Stream must contain : keepalive comment");
    CHECK_EQUAL_TEXT(std::string::npos, receivedData.find("event: endpoint"),
                     "Streamable HTTP GET /mcp must NEVER emit event: endpoint");

    // Check that every data: line contains valid JSON-RPC 2.0
    std::istringstream stream(receivedData);
    std::string line;
    int dataLineCount = 0;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind("data: ", 0) == 0) {
            std::string payload = line.substr(6);
            json dataJson = json::parse(payload, nullptr, false);
            CHECK_FALSE_TEXT(dataJson.is_discarded(), "data line must be valid JSON");
            CHECK_EQUAL_TEXT("2.0", dataJson.value("jsonrpc", ""), "JSON on SSE stream must have jsonrpc 2.0");
            dataLineCount++;
        }
    }
    CHECK_TRUE(dataLineCount > 0);

    // Verify synchronous ping response (id 77) was NOT duplicated onto SSE stream
    CHECK_EQUAL_TEXT(std::string::npos, receivedData.find("\"id\": 77"),
                     "Synchronous POST response must not duplicate to SSE stream");
    CHECK_EQUAL_TEXT(std::string::npos, receivedData.find("\"id\":77"),
                     "Synchronous POST response must not duplicate to SSE stream");

    // Verify non-JSON-RPC mobile payload was NOT leaked onto SSE stream
    CHECK_EQUAL_TEXT(std::string::npos, receivedData.find("mobile_only"),
                     "Non-JSON-RPC mobile notification must not leak onto MCP SSE stream");
}

/*
 * ----------------------------------------------------------------------------
 * TEST_GROUP(McpStreamableHttp_SessionLifecycle)
 * ----------------------------------------------------------------------------
 */
TEST_GROUP(McpStreamableHttp_SessionLifecycle) {
    std::string _ws;
    std::unique_ptr<HistoryStore> _historyStore;
    std::unique_ptr<DynamicToolRegistry> _dynamicRegistry;
    std::unique_ptr<McpServer> _mcpServer;
    std::unique_ptr<WebServer> _webServer;
    int _port = 0;

    void setup() override {
        _ws = makeTempDir("session_life");
        ::setenv("AIMON_CLI_BRAIN_DIR", _ws.c_str(), 1);
        ::setenv("AIMON_IDE_BRAIN_DIR", _ws.c_str(), 1);

        AgentTelemetryDb::getInstance().open(_ws + "/agent_telemetry.db");

        _historyStore = std::make_unique<HistoryStore>();
        _historyStore->open(_ws + "/history.db");

        _dynamicRegistry = std::make_unique<DynamicToolRegistry>();
        json meshTools = json::array({
            {
                {"name", "meshmon_get_node_status"},
                {"description", "LoRa mesh node status"},
                {"inputSchema", {{"type", "object"}}}
            }
        });
        std::vector<std::string> added;
        _dynamicRegistry->registerTools(1, "mesh", meshTools, added);

        _mcpServer = std::make_unique<McpServer>(StateStore::getInstance());
        _mcpServer->setDynamicRegistry(_dynamicRegistry.get());

        WebConfig config;
        config.host = "127.0.0.1";
        config.port = 0;
        config.endpointsEnabled = true;

        _webServer = std::make_unique<WebServer>(
            StateStore::getInstance(), *_historyStore, config, nullptr, _mcpServer.get(), nullptr
        );

        _mcpServer->setNotificationBroadcaster([this](const std::string& notif) {
            if (_webServer) {
                _webServer->broadcastSseNotification(notif);
            }
        });

        CHECK_TRUE(_webServer->start(true));
        _port = _webServer->getPort();
        CHECK_TRUE(_port > 0);
    }

    void teardown() override {
        if (_webServer) {
            _webServer->stop();
            _webServer.reset();
        }
        _mcpServer.reset();
        _dynamicRegistry.reset();
        if (_historyStore) {
            _historyStore->close();
            _historyStore.reset();
        }
        AgentTelemetryDb::getInstance().close();
        std::error_code ec;
        fs::remove_all(_ws, ec);
    }
};

TEST(McpStreamableHttp_SessionLifecycle, WorkspaceAutoScopingAndSessionTracking) {
    httplib::Client cli("127.0.0.1", _port);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(10);

    // 1. Initialize with rootUri pointing to meshmon workspace
    json initReq = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "initialize"},
        {"params", {
            {"protocolVersion", "2025-03-26"},
            {"rootUri", "file:///tmp/work/meshmon"}
        }}
    };
    auto resInit = cli.Post("/mcp", initReq.dump(), "application/json");
    CHECK_TRUE(resInit != nullptr);
    LONGS_EQUAL(200, resInit->status);
    CHECK_TRUE(resInit->has_header("Mcp-Session-Id"));
    std::string sessionId = resInit->get_header_value("Mcp-Session-Id");
    CHECK_FALSE(sessionId.empty());

    // 2. Subsequent POST with Mcp-Session-Id header retains auto-scoped "mesh" profile
    httplib::Headers headers = {{"Mcp-Session-Id", sessionId}};
    json toolsReq = {
        {"jsonrpc", "2.0"},
        {"id", 2},
        {"method", "tools/list"}
    };
    auto resTools = cli.Post("/mcp", headers, toolsReq.dump(), "application/json");
    CHECK_TRUE(resTools != nullptr);
    LONGS_EQUAL(200, resTools->status);

    json toolsJson = json::parse(resTools->body, nullptr, false);
    CHECK_FALSE(toolsJson.is_discarded());
    CHECK_TRUE(toolsJson["result"]["tools"].is_array());
    bool hasMeshTool = false;
    for (const auto& tool : toolsJson["result"]["tools"]) {
        if (tool.value("name", "").rfind("meshmon_", 0) == 0) {
            hasMeshTool = true;
            break;
        }
    }
    CHECK_TRUE_TEXT(hasMeshTool, "Workspace auto-scoping for meshmon must include meshmon_* tools");
}

TEST(McpStreamableHttp_SessionLifecycle, ExplicitDeleteTermination) {
    httplib::Client cli("127.0.0.1", _port);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(10);

    // 1. Create a session
    json initReq = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "initialize"},
        {"params", {{"protocolVersion", "2025-03-26"}}}
    };
    auto resInit = cli.Post("/mcp", initReq.dump(), "application/json");
    CHECK_TRUE(resInit != nullptr);
    LONGS_EQUAL(200, resInit->status);
    std::string sessionId = resInit->get_header_value("Mcp-Session-Id");
    CHECK_FALSE(sessionId.empty());

    // 2. Explicit DELETE /mcp with Mcp-Session-Id header returns 204
    httplib::Headers headers = {{"Mcp-Session-Id", sessionId}};
    auto resDel = cli.Delete("/mcp", headers);
    CHECK_TRUE(resDel != nullptr);
    LONGS_EQUAL(204, resDel->status);

    // 3. Second DELETE with same session ID returns 404
    auto resDel2 = cli.Delete("/mcp", headers);
    CHECK_TRUE(resDel2 != nullptr);
    LONGS_EQUAL(404, resDel2->status);

    // 4. DELETE with unknown session ID returns 404
    httplib::Headers unknownHeaders = {{"Mcp-Session-Id", "unknown-session-12345"}};
    auto resDel3 = cli.Delete("/mcp", unknownHeaders);
    CHECK_TRUE(resDel3 != nullptr);
    LONGS_EQUAL(404, resDel3->status);

    // 5. DELETE with no session ID returns 404
    auto resDel4 = cli.Delete("/mcp");
    CHECK_TRUE(resDel4 != nullptr);
    LONGS_EQUAL(404, resDel4->status);
}

TEST(McpStreamableHttp_SessionLifecycle, IdleReaperAndMaxSessionCap) {
    // 1. Session creation and idle reaper
    auto s1 = _webServer->createMcpSession("test_profile", true);
    CHECK_TRUE(s1 != nullptr);
    CHECK_TRUE(_webServer->getActiveMcpSessionCount() >= 1);

    // Reap sessions with maxIdle = 0 seconds (reaps all sessions whose lastActive <= now)
    size_t reaped = _webServer->reapIdleMcpSessions(std::chrono::seconds(0));
    CHECK_TRUE(reaped >= 1);

    // 2. Max session cap enforcement
    for (size_t i = 0; i < 265; ++i) {
        _webServer->createMcpSession("cap_test", true);
    }
    size_t activeCount = _webServer->getActiveMcpSessionCount();
    CHECK_TRUE(activeCount <= WebServer::MAX_MCP_SESSIONS);
    LONGS_EQUAL(WebServer::MAX_MCP_SESSIONS, activeCount);
}

/*
 * ----------------------------------------------------------------------------
 * TEST_GROUP(McpLegacy_BackwardCompatibility)
 * ----------------------------------------------------------------------------
 */
TEST_GROUP(McpLegacy_BackwardCompatibility) {
    std::string _ws;
    std::unique_ptr<HistoryStore> _historyStore;
    std::unique_ptr<McpServer> _mcpServer;
    std::unique_ptr<WebServer> _webServer;
    int _port = 0;

    void setup() override {
        _ws = makeTempDir("legacy_compat");
        ::setenv("AIMON_CLI_BRAIN_DIR", _ws.c_str(), 1);
        ::setenv("AIMON_IDE_BRAIN_DIR", _ws.c_str(), 1);

        AgentTelemetryDb::getInstance().open(_ws + "/agent_telemetry.db");

        _historyStore = std::make_unique<HistoryStore>();
        _historyStore->open(_ws + "/history.db");

        _mcpServer = std::make_unique<McpServer>(StateStore::getInstance());

        WebConfig config;
        config.host = "127.0.0.1";
        config.port = 0;
        config.endpointsEnabled = true;

        _webServer = std::make_unique<WebServer>(
            StateStore::getInstance(), *_historyStore, config, nullptr, _mcpServer.get(), nullptr
        );

        _mcpServer->setNotificationBroadcaster([this](const std::string& notif) {
            if (_webServer) {
                _webServer->broadcastSseNotification(notif);
            }
        });

        CHECK_TRUE(_webServer->start(true));
        _port = _webServer->getPort();
        CHECK_TRUE(_port > 0);
    }

    void teardown() override {
        if (_webServer) {
            _webServer->stop();
            _webServer.reset();
        }
        _mcpServer.reset();
        if (_historyStore) {
            _historyStore->close();
            _historyStore.reset();
        }
        AgentTelemetryDb::getInstance().close();
        std::error_code ec;
        fs::remove_all(_ws, ec);
    }
};

TEST(McpLegacy_BackwardCompatibility, LegacyHandshakeAndPushDelivery) {
    std::mutex legacyMutex;
    std::condition_variable legacyCv;
    std::string legacyData;
    std::string legacySessionId;
    std::atomic<bool> gotEndpoint{false};
    std::atomic<bool> gotInitPush{false};
    std::atomic<bool> gotToolsPush{false};
    std::atomic<bool> legacyStop{false};

    // 1. Connect to GET /sse to establish legacy SSE stream
    std::thread legacyReader([&]() {
        httplib::Client cli("127.0.0.1", _port);
        cli.set_read_timeout(10);
        httplib::Headers headers = {{"Accept", "text/event-stream"}};
        cli.Get("/sse", headers, [&](const char* data, size_t len) {
            {
                std::lock_guard<std::mutex> lk(legacyMutex);
                legacyData.append(data, len);

                if (!gotEndpoint.load()) {
                    const std::string prefix = "event: endpoint\ndata: /message?sessionId=";
                    size_t epPos = legacyData.find(prefix);
                    if (epPos != std::string::npos) {
                        size_t start = epPos + prefix.length();
                        size_t end = legacyData.find_first_of("&\n\r", start);
                        if (end != std::string::npos) {
                            legacySessionId = legacyData.substr(start, end - start);
                            gotEndpoint = true;
                        }
                    }
                }

                if (legacyData.find("\"id\":100") != std::string::npos ||
                    legacyData.find("\"id\": 100") != std::string::npos) {
                    gotInitPush = true;
                }
                if (legacyData.find("\"id\":101") != std::string::npos ||
                    legacyData.find("\"id\": 101") != std::string::npos) {
                    gotToolsPush = true;
                }
            }
            legacyCv.notify_all();
            return !legacyStop.load();
        });
    });

    // 2. Wait for endpoint announcement
    {
        std::unique_lock<std::mutex> lk(legacyMutex);
        legacyCv.wait_for(lk, std::chrono::seconds(5), [&]() {
            return gotEndpoint.load();
        });
    }
    CHECK_TRUE_TEXT(gotEndpoint.load(), "Did not receive legacy event: endpoint on GET /sse");
    CHECK_FALSE(legacySessionId.empty());

    httplib::Client postCli("127.0.0.1", _port);
    postCli.set_connection_timeout(5);
    postCli.set_read_timeout(10);

    // 3. Send initialize to POST /message?sessionId=...
    json initReq = {
        {"jsonrpc", "2.0"},
        {"id", 100},
        {"method", "initialize"},
        {"params", {
            {"protocolVersion", "2024-11-05"},
            {"clientInfo", {{"name", "legacy-client"}, {"version", "0.9"}}}
        }}
    };
    std::string postInitPath = "/message?sessionId=" + legacySessionId;
    auto resInit = postCli.Post(postInitPath, initReq.dump(), "application/json");
    CHECK_TRUE(resInit != nullptr);
    LONGS_EQUAL(200, resInit->status);

    json initResp = json::parse(resInit->body, nullptr, false);
    CHECK_FALSE(initResp.is_discarded());
    CHECK_EQUAL("2024-11-05", initResp["result"].value("protocolVersion", ""));

    // Verify response was also pushed to the legacy SSE stream
    {
        std::unique_lock<std::mutex> lk(legacyMutex);
        legacyCv.wait_for(lk, std::chrono::seconds(5), [&]() {
            return gotInitPush.load();
        });
    }
    CHECK_TRUE_TEXT(gotInitPush.load(), "Legacy SSE did not push initialize response frame");

    // 4. Send tools/list to POST /message?sessionId=...
    json toolsReq = {
        {"jsonrpc", "2.0"},
        {"id", 101},
        {"method", "tools/list"}
    };
    auto resTools = postCli.Post(postInitPath, toolsReq.dump(), "application/json");
    CHECK_TRUE(resTools != nullptr);
    LONGS_EQUAL(200, resTools->status);

    // Verify tools/list response was pushed to legacy SSE stream
    {
        std::unique_lock<std::mutex> lk(legacyMutex);
        legacyCv.wait_for(lk, std::chrono::seconds(5), [&]() {
            return gotToolsPush.load();
        });
    }
    CHECK_TRUE_TEXT(gotToolsPush.load(), "Legacy SSE did not push tools/list response frame");

    // 5. Clean shutdown
    legacyStop = true;
    _webServer->terminateMcpSession(legacySessionId);
    if (legacyReader.joinable()) {
        legacyReader.join();
    }

    CHECK_TRUE(legacyData.find("event: endpoint") != std::string::npos);
    CHECK_TRUE(legacyData.find("event: message") != std::string::npos);
}

/*
 * ----------------------------------------------------------------------------
 * main() with --negative-control Verification Flag
 * ----------------------------------------------------------------------------
 */
int main(int argc, char** argv) {
    MemoryLeakWarningPlugin::turnOffNewDeleteOverloads();

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--negative-control") == 0) {
            std::string ws = makeTempDir("neg_ctrl");
            ::setenv("AIMON_CLI_BRAIN_DIR", ws.c_str(), 1);
            ::setenv("AIMON_IDE_BRAIN_DIR", ws.c_str(), 1);

            HistoryStore hstore;
            hstore.open(ws + "/history.db");
            McpServer mcp(StateStore::getInstance());

            WebConfig wcfg;
            wcfg.host = "127.0.0.1";
            wcfg.port = 0;
            wcfg.endpointsEnabled = true;

            WebServer server(StateStore::getInstance(), hstore, wcfg, nullptr, &mcp, nullptr);
            bool invalidJsonRejected = false;
            bool deleteUnknownRejected = false;

            if (server.start(true)) {
                httplib::Client cli("127.0.0.1", server.getPort());
                cli.set_connection_timeout(2);
                cli.set_read_timeout(5);

                // 1. Deliberate negative control: Malformed JSON must return 400 with -32700
                auto res1 = cli.Post("/mcp", "{bad json", "application/json");
                if (res1 && res1->status == 400) {
                    json err1 = json::parse(res1->body, nullptr, false);
                    if (!err1.is_discarded() && err1.value("error", json::object()).value("code", 0) == -32700) {
                        invalidJsonRejected = true;
                    }
                }

                // 2. Deliberate negative control: DELETE /mcp with non-existent session ID must return 404
                httplib::Headers hDel = {{"Mcp-Session-Id", "non-existent-negative-session-id"}};
                auto res2 = cli.Delete("/mcp", hDel);
                if (res2 && res2->status == 404) {
                    deleteUnknownRejected = true;
                }

                server.stop();
            }

            hstore.close();
            std::error_code ec;
            fs::remove_all(ws, ec);

            if (invalidJsonRejected && deleteUnknownRejected) {
                std::cerr << "[NEGATIVE-CONTROL] Deliberate negative control verified: "
                          << "malformed JSON returned HTTP 400 (-32700) and unknown session DELETE returned HTTP 404; "
                          << "exiting 1 as expected.\n";
                return 1;
            }
            std::cerr << "[NEGATIVE-CONTROL] Negative control conditions were not met.\n";
            return 0;
        }
    }

    return CommandLineTestRunner::RunAllTests(argc, argv);
}

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
