/*
 * TestWebServerApi.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <cassert>
#include <iostream>
#include <thread>
#include <chrono>
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include <httplib.h>
#include "WebServer.hxx"
#include "StateStore.hxx"
#include "HistoryStore.hxx"
#include "AgentTelemetryDb.hxx"
#include "MobileGateway.hxx"

using namespace aimon;
using json = nlohmann::json;

int main() {
    std::cout << "=== Starting TestWebServerApi ===" << std::endl;

    std::string testDbPath = "/tmp/test_web_server_api.db";
    AgentTelemetryDb::getInstance().open(testDbPath);

    StateStore& stateStore = StateStore::getInstance();
    HistoryStore historyStore;

    WebConfig config;
    config.host = "127.0.0.1";
    config.port = 3899; // test port
    config.endpointsEnabled = true;

    WebServer server(stateStore, historyStore, config);
    assert(server.start(true)); // Start asynchronously


    // Allow server to bind
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    httplib::Client client("http://127.0.0.1:3899");
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(5, 0);

    // 1. Status API
    auto resStatus = client.Get("/api/status");
    assert(resStatus && resStatus->status == 200);

    // 2. Ingest Telemetry Events
    json evStart = {
        {"event_type", "SESSION_START"},
        {"session_id", "test-web-sess-1"},
        {"agent_type", "antigravity"},
        {"workspace", "/workspace/aimon"},
        {"model", "Antigravity"},
        {"timestamp", static_cast<int64_t>(time(nullptr))}
    };
    auto resStart = client.Post("/api/telemetry/event", evStart.dump(), "application/json");
    assert(resStart && resStart->status == 200);

    json evTool = {
        {"event_type", "TOOL_POST_USE"},
        {"session_id", "test-web-sess-1"},
        {"agent_type", "antigravity"},
        {"tool_name", "grep_search"},
        {"duration_ms", 45.0},
        {"status", "OK"},
        {"timestamp", static_cast<int64_t>(time(nullptr))}
    };
    auto resTool = client.Post("/api/telemetry/event", evTool.dump(), "application/json");
    assert(resTool && resTool->status == 200);

    json evEnd = {
        {"event_type", "SESSION_END"},
        {"session_id", "test-web-sess-1"},
        {"status", "COMPLETED"},
        {"total_turns", 1},
        {"prompt_tokens", 1000},
        {"comp_tokens", 200},
        {"tool_calls", 1},
        {"errors", 0},
        {"avg_turn_ms", 1500.0},
        {"timestamp", static_cast<int64_t>(time(nullptr))}
    };
    auto resEnd = client.Post("/api/telemetry/event", evEnd.dump(), "application/json");
    assert(resEnd && resEnd->status == 200);

    // 3. Query Telemetry APIs
    auto resOverview = client.Get("/api/telemetry/overview");
    assert(resOverview && resOverview->status == 200);
    json ov = json::parse(resOverview->body);
    assert(ov.contains("total_sessions"));

    auto resTimeseries = client.Get("/api/telemetry/timeseries?window=1h");
    assert(resTimeseries && resTimeseries->status == 200);

    auto resSessions = client.Get("/api/telemetry/sessions");
    assert(resSessions && resSessions->status == 200);

    auto resTools = client.Get("/api/telemetry/tools");
    assert(resTools && resTools->status == 200);

    // 4. Mobile Pairing APIs
    auto resQr = client.Get("/api/mobile/qr");
    assert(resQr && resQr->status == 200);
    json qr = json::parse(resQr->body);
    assert(qr.contains("secret"));
    std::string secret = qr["secret"];

    json pairBody = {
        {"secret", secret},
        {"device_id", "phone-alpha"},
        {"device_name", "Pixel 9 Pro"}
    };
    auto resPair = client.Post("/api/mobile/pair", pairBody.dump(), "application/json");
    assert(resPair && resPair->status == 200);
    json pairResp = json::parse(resPair->body);
    assert(pairResp.contains("token"));

    auto resDevs = client.Get("/api/mobile/devices");
    assert(resDevs && resDevs->status == 200);
    json devs = json::parse(resDevs->body);
    assert(devs.size() == 1);

    // 5. Action Approval Request & Asynchronous Decision API
    std::thread approverThread([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        httplib::Client mobileClient("http://127.0.0.1:3899");
        mobileClient.set_connection_timeout(2, 0);
        mobileClient.set_read_timeout(5, 0);

        // Poll pending
        auto resPending = mobileClient.Get("/api/approvals/pending");
        assert(resPending && resPending->status == 200);
        json pendingList = json::parse(resPending->body);
        assert(!pendingList.empty());
        std::string approvalId = pendingList[0]["approval_id"];

        // Submit decision
        json decBody = {
            {"approval_id", approvalId},
            {"decision", "allow"}
        };
        auto resDec = mobileClient.Post("/api/approvals/decision", decBody.dump(), "application/json");
        assert(resDec && resDec->status == 200);
    });


    json reqBody = {
        {"agent_type", "antigravity"},
        {"tool_name", "run_command"},
        {"workspace", "/workspace/aimon"},
        {"tool_args", {{"CommandLine", "make"}}},
        {"reason", "Test compilation approval"},
        {"timeout_seconds", 5}
    };

    auto resAppr = client.Post("/api/approvals/request", reqBody.dump(), "application/json");
    assert(resAppr && resAppr->status == 200);
    json apprResp = json::parse(resAppr->body);
    assert(apprResp.value("verdict", "") == "APPROVED");

    approverThread.join();

    server.stop();
    std::cout << "=== TestWebServerApi: ALL TESTS PASSED ===" << std::endl;
    return 0;
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
