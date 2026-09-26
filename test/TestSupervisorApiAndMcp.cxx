/*
 * TestSupervisorApiAndMcp.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <chrono>
#include <thread>
#include <string>
#include <vector>
#include <memory>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include <httplib.h>

#include "WebServer.hxx"
#include "McpServer.hxx"
#include "ServiceSupervisor.hxx"
#include "StateStore.hxx"
#include "HistoryStore.hxx"
#include "AgentTelemetryDb.hxx"

#include <CppUTest/TestHarness.h>
#include <CppUTest/CommandLineTestRunner.h>

using namespace aimon;
using json = nlohmann::json;

static SupervisorConfig createTestSupervisorConfig() {
    SupervisorConfig sc;
    sc.enabled = true;
    sc.probeIntervalSec = 30;
    sc.probeTimeoutSec = 2;
    sc.crashLoopWindowSec = 60;
    sc.crashLoopMaxRetries = 3;
    return sc;
}

static std::vector<SupervisedServiceConfig> createTestServices() {
    std::vector<SupervisedServiceConfig> list;

    SupervisedServiceConfig svc1;
    svc1.id = "netmon-svc";
    svc1.name = "Netmon Service";
    svc1.host = "127.0.0.1";
    svc1.port = 18881;
    svc1.secondaryPort = 18882;
    svc1.startCmd = "true";
    svc1.enabled = true;
    list.push_back(svc1);

    SupervisedServiceConfig svc2;
    svc2.id = "meshmon-svc";
    svc2.name = "Meshmon Service";
    svc2.host = "127.0.0.1";
    svc2.port = 18883;
    svc2.secondaryPort = 0;
    svc2.startCmd = "true";
    svc2.enabled = true;
    list.push_back(svc2);

    return list;
}

// ============================================================================
// Unit Tests: McpServer Fleet Tools Dispatch & Schema Validation
// ============================================================================

TEST_GROUP(SupervisorMcp_Unit) {
    StateStore* stateStore = nullptr;
    ServiceSupervisor* supervisor = nullptr;
    McpServer* mcpServer = nullptr;

    void setup() override {
        stateStore = &StateStore::getInstance();
        supervisor = new ServiceSupervisor(createTestSupervisorConfig(), createTestServices());
        mcpServer = new McpServer(*stateStore);
    }

    void teardown() override {
        delete mcpServer;
        mcpServer = nullptr;
        delete supervisor;
        supervisor = nullptr;
    }
};

TEST(SupervisorMcp_Unit, ToolsListIncludesSupervisorTools) {
    json req = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "tools/list"},
        {"params", json::object()}
    };

    json res = mcpServer->handleMessage(req);
    CHECK_TRUE(res.contains("result"));
    CHECK_TRUE(res["result"].contains("tools"));
    auto& tools = res["result"]["tools"];

    bool foundList = false;
    bool foundStatus = false;
    bool foundRestart = false;
    bool foundLogs = false;

    for (const auto& t : tools) {
        std::string name = t["name"];
        if (name == "service_list") foundList = true;
        else if (name == "service_status") {
            foundStatus = true;
            CHECK_TRUE(t["inputSchema"]["properties"].contains("service"));
        } else if (name == "service_restart") {
            foundRestart = true;
            CHECK_TRUE(t["inputSchema"]["properties"].contains("service"));
            CHECK_TRUE(t["inputSchema"]["properties"].contains("force"));
        } else if (name == "service_get_logs") {
            foundLogs = true;
            CHECK_TRUE(t["inputSchema"]["properties"].contains("service"));
            CHECK_TRUE(t["inputSchema"]["properties"].contains("lines"));
        }
    }

    CHECK_TRUE(foundList);
    CHECK_TRUE(foundStatus);
    CHECK_TRUE(foundRestart);
    CHECK_TRUE(foundLogs);
}

TEST(SupervisorMcp_Unit, ServiceListWithoutSupervisor) {
    mcpServer->setServiceSupervisor(nullptr);
    json req = {
        {"jsonrpc", "2.0"},
        {"id", 2},
        {"method", "tools/call"},
        {"params", {{"name", "service_list"}, {"arguments", json::object()}}}
    };

    json res = mcpServer->handleMessage(req);
    CHECK_TRUE(res.contains("result"));
    std::string text = res["result"]["content"][0]["text"];
    CHECK_TRUE(text.find("not enabled or not running") != std::string::npos);
}

TEST(SupervisorMcp_Unit, ServiceListWithSupervisor) {
    mcpServer->setServiceSupervisor(supervisor);
    json req = {
        {"jsonrpc", "2.0"},
        {"id", 3},
        {"method", "tools/call"},
        {"params", {{"name", "service_list"}, {"arguments", json::object()}}}
    };

    json res = mcpServer->handleMessage(req);
    CHECK_TRUE(res.contains("result"));
    std::string text = res["result"]["content"][0]["text"];
    CHECK_TRUE(text.find("netmon-svc") != std::string::npos);
    CHECK_TRUE(text.find("meshmon-svc") != std::string::npos);
}

TEST(SupervisorMcp_Unit, ServiceStatusMissingArg) {
    mcpServer->setServiceSupervisor(supervisor);
    json req = {
        {"jsonrpc", "2.0"},
        {"id", 4},
        {"method", "tools/call"},
        {"params", {{"name", "service_status"}, {"arguments", json::object()}}}
    };

    json res = mcpServer->handleMessage(req);
    CHECK_TRUE(res.contains("error"));
    LONGS_EQUAL(-32602, res["error"]["code"].get<int>());
}

TEST(SupervisorMcp_Unit, ServiceStatusUnknownService) {
    mcpServer->setServiceSupervisor(supervisor);
    json req = {
        {"jsonrpc", "2.0"},
        {"id", 5},
        {"method", "tools/call"},
        {"params", {{"name", "service_status"}, {"arguments", {{"service", "non-existent"}}}}}
    };

    json res = mcpServer->handleMessage(req);
    CHECK_TRUE(res.contains("error"));
    LONGS_EQUAL(-32602, res["error"]["code"].get<int>());
}

TEST(SupervisorMcp_Unit, ServiceStatusSuccess) {
    mcpServer->setServiceSupervisor(supervisor);
    json req = {
        {"jsonrpc", "2.0"},
        {"id", 6},
        {"method", "tools/call"},
        {"params", {{"name", "service_status"}, {"arguments", {{"service", "netmon-svc"}}}}}
    };

    json res = mcpServer->handleMessage(req);
    CHECK_TRUE(res.contains("result"));
    std::string text = res["result"]["content"][0]["text"];
    json parsed = json::parse(text);
    CHECK_TRUE(parsed.contains("id"));
    STRCMP_EQUAL("netmon-svc", parsed["id"].get<std::string>().c_str());
    CHECK_TRUE(parsed.contains("state"));
}

TEST(SupervisorMcp_Unit, ServiceRestartMissingArg) {
    mcpServer->setServiceSupervisor(supervisor);
    json req = {
        {"jsonrpc", "2.0"},
        {"id", 7},
        {"method", "tools/call"},
        {"params", {{"name", "service_restart"}, {"arguments", json::object()}}}
    };

    json res = mcpServer->handleMessage(req);
    CHECK_TRUE(res.contains("error"));
    LONGS_EQUAL(-32602, res["error"]["code"].get<int>());
}

TEST(SupervisorMcp_Unit, ServiceRestartSuccess) {
    mcpServer->setServiceSupervisor(supervisor);
    json req = {
        {"jsonrpc", "2.0"},
        {"id", 8},
        {"method", "tools/call"},
        {"params", {{"name", "service_restart"}, {"arguments", {{"service", "netmon-svc"}, {"force", false}}}}}
    };

    json res = mcpServer->handleMessage(req);
    CHECK_TRUE(res.contains("result"));
    std::string text = res["result"]["content"][0]["text"];
    CHECK_TRUE(text.find("Successfully dispatched restart") != std::string::npos);
}

TEST(SupervisorMcp_Unit, ServiceGetLogsMissingArg) {
    mcpServer->setServiceSupervisor(supervisor);
    json req = {
        {"jsonrpc", "2.0"},
        {"id", 9},
        {"method", "tools/call"},
        {"params", {{"name", "service_get_logs"}, {"arguments", json::object()}}}
    };

    json res = mcpServer->handleMessage(req);
    CHECK_TRUE(res.contains("error"));
    LONGS_EQUAL(-32602, res["error"]["code"].get<int>());
}

TEST(SupervisorMcp_Unit, ServiceGetLogsSuccess) {
    mcpServer->setServiceSupervisor(supervisor);
    json req = {
        {"jsonrpc", "2.0"},
        {"id", 10},
        {"method", "tools/call"},
        {"params", {{"name", "service_get_logs"}, {"arguments", {{"service", "netmon-svc"}, {"lines", 10}}}}}
    };

    json res = mcpServer->handleMessage(req);
    CHECK_TRUE(res.contains("result"));
    CHECK_TRUE(res["result"]["content"][0].contains("text"));
}

// ============================================================================
// Integrated Tests: Live HTTP REST Endpoints on Ephemeral Loopback
// ============================================================================

TEST_GROUP(SupervisorRestApi_Integrated) {
    StateStore* stateStore = nullptr;
    HistoryStore* historyStore = nullptr;
    ServiceSupervisor* supervisor = nullptr;
    WebServer* webServer = nullptr;
    int boundPort = 0;

    void setup() override {
        stateStore = &StateStore::getInstance();
        historyStore = new HistoryStore();
        supervisor = new ServiceSupervisor(createTestSupervisorConfig(), createTestServices());

        WebConfig cfg;
        cfg.host = "127.0.0.1";
        cfg.port = 0; // Ephemeral port
        cfg.endpointsEnabled = true;

        webServer = new WebServer(*stateStore, *historyStore, cfg);
        webServer->setServiceSupervisor(supervisor);
        CHECK_TRUE(webServer->start(true));

        boundPort = webServer->getPort();
        CHECK_TRUE(boundPort > 0);
    }

    void teardown() override {
        if (webServer) {
            webServer->stop();
            delete webServer;
            webServer = nullptr;
        }
        if (supervisor) {
            supervisor->stop();
            delete supervisor;
            supervisor = nullptr;
        }
        if (historyStore) {
            delete historyStore;
            historyStore = nullptr;
        }
    }
};

TEST(SupervisorRestApi_Integrated, GetServicesReturns200) {
    httplib::Client client("http://127.0.0.1:" + std::to_string(boundPort));
    httplib::Headers headers = { {"User-Agent", "aimon-hook/1.0"} };

    auto res = client.Get("/api/services", headers);
    CHECK_TRUE(res != nullptr);
    LONGS_EQUAL(200, res->status);

    json body = json::parse(res->body);
    CHECK_TRUE(body.contains("services"));
    CHECK_TRUE(body["services"].is_array());
    LONGS_EQUAL(2, body["services"].size());

    bool foundNetmon = false;
    bool foundMeshmon = false;
    for (const auto& s : body["services"]) {
        std::string id = s["id"];
        if (id == "netmon-svc") foundNetmon = true;
        if (id == "meshmon-svc") foundMeshmon = true;
    }
    CHECK_TRUE(foundNetmon);
    CHECK_TRUE(foundMeshmon);
}

TEST(SupervisorRestApi_Integrated, GetSupervisorStatusReturns200) {
    httplib::Client client("http://127.0.0.1:" + std::to_string(boundPort));
    httplib::Headers headers = { {"User-Agent", "aimon-hook/1.0"} };

    auto res = client.Get("/api/supervisor/status", headers);
    CHECK_TRUE(res != nullptr);
    LONGS_EQUAL(200, res->status);

    json body = json::parse(res->body);
    CHECK_TRUE(body.contains("running"));
    CHECK_TRUE(body.contains("services_count"));
    LONGS_EQUAL(2, body["services_count"].get<int>());
}

TEST(SupervisorRestApi_Integrated, PostRestartMissingParamReturns400) {
    httplib::Client client("http://127.0.0.1:" + std::to_string(boundPort));
    httplib::Headers headers = { {"User-Agent", "aimon-hook/1.0"} };

    auto res = client.Post("/api/services/restart", headers, "{}", "application/json");
    CHECK_TRUE(res != nullptr);
    LONGS_EQUAL(400, res->status);

    json body = json::parse(res->body);
    CHECK_TRUE(body.contains("error"));
}

TEST(SupervisorRestApi_Integrated, PostRestartUnknownServiceReturns400) {
    httplib::Client client("http://127.0.0.1:" + std::to_string(boundPort));
    httplib::Headers headers = { {"User-Agent", "aimon-hook/1.0"} };

    json req = {{"service", "non-existent-svc"}};
    auto res = client.Post("/api/services/restart", headers, req.dump(), "application/json");
    CHECK_TRUE(res != nullptr);
    LONGS_EQUAL(400, res->status);

    json body = json::parse(res->body);
    CHECK_FALSE(body["success"].get<bool>());
}

TEST(SupervisorRestApi_Integrated, PostRestartSuccessReturns200) {
    httplib::Client client("http://127.0.0.1:" + std::to_string(boundPort));
    httplib::Headers headers = { {"User-Agent", "aimon-hook/1.0"} };

    json req = {{"service", "netmon-svc"}, {"force", false}};
    auto res = client.Post("/api/services/restart", headers, req.dump(), "application/json");
    CHECK_TRUE(res != nullptr);
    LONGS_EQUAL(200, res->status);

    json body = json::parse(res->body);
    CHECK_TRUE(body["success"].get<bool>());
    STRCMP_EQUAL("netmon-svc", body["service"].get<std::string>().c_str());
}

TEST(SupervisorRestApi_Integrated, GetLogsMissingParamReturns400) {
    httplib::Client client("http://127.0.0.1:" + std::to_string(boundPort));
    httplib::Headers headers = { {"User-Agent", "aimon-hook/1.0"} };

    auto res = client.Get("/api/services/logs", headers);
    CHECK_TRUE(res != nullptr);
    LONGS_EQUAL(400, res->status);
}

TEST(SupervisorRestApi_Integrated, GetLogsSuccessReturns200) {
    httplib::Client client("http://127.0.0.1:" + std::to_string(boundPort));
    httplib::Headers headers = { {"User-Agent", "aimon-hook/1.0"} };

    auto res = client.Get("/api/services/logs?service=netmon-svc&lines=20", headers);
    CHECK_TRUE(res != nullptr);
    LONGS_EQUAL(200, res->status);

    json body = json::parse(res->body);
    CHECK_TRUE(body.contains("service"));
    STRCMP_EQUAL("netmon-svc", body["service"].get<std::string>().c_str());
    CHECK_TRUE(body.contains("logs"));
}

TEST(SupervisorRestApi_Integrated, UnsetSupervisorReturns503) {
    webServer->setServiceSupervisor(nullptr);
    httplib::Client client("http://127.0.0.1:" + std::to_string(boundPort));
    httplib::Headers headers = { {"User-Agent", "aimon-hook/1.0"} };

    json req = {{"service", "netmon-svc"}};
    auto res = client.Post("/api/services/restart", headers, req.dump(), "application/json");
    CHECK_TRUE(res != nullptr);
    LONGS_EQUAL(503, res->status);

    auto resLogs = client.Get("/api/services/logs?service=netmon-svc", headers);
    CHECK_TRUE(resLogs != nullptr);
    LONGS_EQUAL(503, resLogs->status);
}

int main(int argc, char** argv) {
    // Pre-warm cpp-httplib internal thread_local regexes and state before CppUTest memory tracking
    {
        httplib::Server s;
        s.Get("/warmup", [](const httplib::Request&, httplib::Response& res) {
            res.set_content("ok", "text/plain");
        });
        int port = s.bind_to_any_port("127.0.0.1");
        std::thread t([&]() { s.listen_after_bind(); });
        s.wait_until_ready();
        httplib::Client cli("http://127.0.0.1:" + std::to_string(port));
        cli.Get("/warmup?p=1");
        s.stop();
        if (t.joinable()) t.join();
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
