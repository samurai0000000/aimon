/*
 * TestWebServerApi.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <atomic>
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

    // 1. Status API with Enterprise Fast Requests
    AggregateStatus aggStatus;
    aggStatus.cursor.isAuthenticated = true;
    aggStatus.cursor.planTier = "Enterprise";
    aggStatus.cursor.usageMode = "requests";
    aggStatus.cursor.fastRequestsUsed = 12206;
    aggStatus.cursor.fastRequestsLimit = 120000;
    DailySpendPoint pt;
    pt.dayStr = "2026-10-02";
    pt.requestsUsed = 706;
    pt.cumulativeRequests = 12206;
    aggStatus.cursor.dailySpendHistory.push_back(pt);
    stateStore.update(aggStatus);

    auto resStatus = client.Get("/api/status");
    assert(resStatus && resStatus->status == 200);
    json statusJson = json::parse(resStatus->body);
    assert(statusJson.contains("cursor"));
    assert(statusJson["cursor"].contains("usage_mode") && statusJson["cursor"]["usage_mode"] == "requests");
    assert(statusJson["cursor"].contains("fast_requests_used") && statusJson["cursor"]["fast_requests_used"] == 12206);
    assert(statusJson["cursor"].contains("daily_spend") && statusJson["cursor"]["daily_spend"].size() == 1);
    assert(statusJson["cursor"]["daily_spend"][0]["requests_used"] == 706);
    assert(statusJson["cursor"]["daily_spend"][0]["cumulative_requests"] == 12206);

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
    std::string mobileToken = pairResp["token"];
    httplib::Headers mobileAuthHeaders = {{"X-Mobile-Token", mobileToken}};

    auto resDevs = client.Get("/api/mobile/devices", mobileAuthHeaders);
    assert(resDevs && resDevs->status == 200);
    json devs = json::parse(resDevs->body);
    assert(devs.size() == 1);

    // 5. Locked Terminal Conversation Port Pinning, Anti-Hijacking & Stale Port Pruning
    {
        namespace fs = std::filesystem;
        fs::path tmpDir = "/tmp/test_webserver_agy_pinning";
        fs::remove_all(tmpDir);
        fs::create_directories(tmpDir / "cli_brain");
        fs::create_directories(tmpDir / "ide_brain");

        const int wrongPort = 39111;
        const int lockedPort = 39112;
        const std::string wrongCsrf = "csrf-wrong-39111";
        const std::string lockedCsrf = "csrf-locked-39112";
        const std::string lockedCid = "locked-conv-uuid-777";

        std::atomic<int> wrongSendCount{0};
        std::atomic<int> wrongStartCascadeCount{0};
        std::atomic<int> wrongMetadataCount{0};
        std::atomic<int> lockedSendCount{0};
        std::atomic<int> lockedTrajectoryCount{0};

        auto attachMockLsHandlers = [](httplib::Server& srv,
                                       const std::string& expectedCsrf,
                                       const std::string& trajectoryText,
                                       std::atomic<int>& sendCounter,
                                       std::atomic<int>* startCounter,
                                       std::atomic<int>* metaCounter,
                                       std::atomic<int>* trajCounter) {
            srv.Post("/exa.language_server_pb.LanguageServerService/GetConversationMetadata",
                     [expectedCsrf, metaCounter](const httplib::Request& req, httplib::Response& res) {
                if (req.get_header_value("x-codeium-csrf-token") != expectedCsrf) {
                    res.status = 403;
                    return;
                }
                if (metaCounter) (*metaCounter)++;
                // Simulate shared ~/.gemini/antigravity-cli/conversations/ disk returning 200 OK on ANY agy process
                res.set_content("{\"metadata\":{\"title\":\"Shared CLI Conversation\"}}", "application/json");
            });

            srv.Post("/exa.language_server_pb.LanguageServerService/GetCascadeModelConfigData",
                     [expectedCsrf](const httplib::Request& req, httplib::Response& res) {
                if (req.get_header_value("x-codeium-csrf-token") != expectedCsrf) {
                    res.status = 403;
                    return;
                }
                json cfg = {
                    {"clientModelConfigs", json::array({
                        {
                            {"label", "Gemini 3.1 Pro (High)"},
                            {"modelOrAlias", {{"model", "MODEL_PLACEHOLDER_M418"}}}
                        }
                    })},
                    {"defaultOverrideModelConfig", {
                        {"modelOrAlias", {{"model", "MODEL_PLACEHOLDER_M418"}}}
                    }}
                };
                res.set_content(cfg.dump(), "application/json");
            });

            srv.Post("/exa.language_server_pb.LanguageServerService/StartCascade",
                     [expectedCsrf, startCounter](const httplib::Request& req, httplib::Response& res) {
                if (req.get_header_value("x-codeium-csrf-token") != expectedCsrf) {
                    res.status = 403;
                    return;
                }
                if (startCounter) (*startCounter)++;
                res.set_content("{\"cascadeId\":\"hijacked-new-cascade\"}", "application/json");
            });

            srv.Post("/exa.language_server_pb.LanguageServerService/SendUserCascadeMessage",
                     [expectedCsrf, &sendCounter](const httplib::Request& req, httplib::Response& res) {
                if (req.get_header_value("x-codeium-csrf-token") != expectedCsrf) {
                    res.status = 403;
                    return;
                }
                sendCounter++;
                res.set_content("{\"ok\":true}", "application/json");
            });

            srv.Post("/exa.language_server_pb.LanguageServerService/GetCascadeTrajectory",
                     [expectedCsrf, trajectoryText, trajCounter](const httplib::Request& req, httplib::Response& res) {
                if (req.get_header_value("x-codeium-csrf-token") != expectedCsrf) {
                    res.status = 403;
                    return;
                }
                if (trajCounter) (*trajCounter)++;
                json tj = {
                    {"trajectory", {
                        {"generatorMetadata", json::array({
                            {{"chatModel", {{"model", "MODEL_PLACEHOLDER_M418"}}}}
                        })},
                        {"steps", json::array({
                            {
                                {"type", "CORTEX_STEP_TYPE_USER_INPUT"},
                                {"userInput", {{"userResponse", "Ping terminal"}}}
                            },
                            {
                                {"type", "CORTEX_STEP_TYPE_PLANNER_RESPONSE"},
                                {"plannerResponse", {
                                    {"thinking", "Thinking in TUI"},
                                    {"modifiedResponse", trajectoryText}
                                }}
                            }
                        })}
                    }}
                };
                res.set_content(tj.dump(), "application/json");
            });
        };

        httplib::Server wrongLs;
        httplib::Server lockedLs;
        attachMockLsHandlers(wrongLs, wrongCsrf, "WRONG_PROCESS_STEP",
                             wrongSendCount, &wrongStartCascadeCount, &wrongMetadataCount, nullptr);
        attachMockLsHandlers(lockedLs, lockedCsrf, "LOCKED_TUI_LIVE_STEP",
                             lockedSendCount, nullptr, nullptr, &lockedTrajectoryCount);

        std::thread wrongThread([&]() { wrongLs.listen("127.0.0.1", wrongPort); });
        std::thread lockedThread([&]() { lockedLs.listen("127.0.0.1", lockedPort); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        fs::path procsFile = tmpDir / "agy_processes.json";
        fs::path endpointsFile = tmpDir / "agy_ls_endpoints.json";
        fs::path fakeAgyBin = tmpDir / "fake_agy_fail.sh";
        {
            std::ofstream f(fakeAgyBin);
            f << "#!/bin/sh\nexit 1\n";
        }
        fs::permissions(fakeAgyBin, fs::perms::owner_all);

        json procsJson = json::array({
            {
                {"pid", 9001},
                {"httpPort", wrongPort},
                {"httpsPort", 0},
                {"cwd", "/workspace/aimon"},
                {"lockedConversationId", "other-conv-uuid-111"},
                {"terminalPts", "pts/2"}
            },
            {
                {"pid", 9002},
                {"httpPort", lockedPort},
                {"httpsPort", 0},
                {"cwd", "/tmp"},
                {"lockedConversationId", lockedCid},
                {"terminalPts", "pts/7"}
            }
        });
        {
            std::ofstream pf(procsFile);
            pf << procsJson.dump(2);
        }

        // Seed endpoints file with ONLY wrongPort + a dead stale port (59999), omitting lockedPort
        json initialEndpoints = {
            {"endpoints", {
                {std::to_string(wrongPort), {
                    {"port", wrongPort},
                    {"csrf_token", wrongCsrf},
                    {"updated_at", 1700000000}
                }},
                {"59999", {
                    {"port", 59999},
                    {"csrf_token", "dead-stale-token"},
                    {"updated_at", 1700000000}
                }}
            }}
        };
        {
            std::ofstream ef(endpointsFile);
            ef << initialEndpoints.dump(2);
        }

        setenv("AIMON_CLI_BRAIN_DIR", (tmpDir / "cli_brain").c_str(), 1);
        setenv("AIMON_IDE_BRAIN_DIR", (tmpDir / "ide_brain").c_str(), 1);
        setenv("AIMON_AGY_PROCESSES_FILE", procsFile.c_str(), 1);
        setenv("AIMON_AGY_ENDPOINTS_FILE", endpointsFile.c_str(), 1);
        setenv("AIMON_AGY_BIN", fakeAgyBin.c_str(), 1);
        unsetenv("AGY_LS_PORT");
        unsetenv("AGY_LS_CSRF_TOKEN");

        // 6a. When lockedPort's CSRF token is missing, prompt MUST NOT hijack onto wrongPort or start a new cascade
        json promptReq = {
            {"agentId", "antigravity"},
            {"conversationId", lockedCid},
            {"source", "cli"},
            {"model", "Gemini 3.1 Pro (High)"},
            {"prompt", "Should fail safely without hijacking wrongPort"}
        };
        auto resMissingTok = client.Post("/api/agent/prompt", mobileAuthHeaders, promptReq.dump(), "application/json");
        assert(resMissingTok && resMissingTok->status == 503);
        json missingTokJson = json::parse(resMissingTok->body);
        assert(missingTokJson.value("error", "").find("pts/7") != std::string::npos);
        assert(wrongSendCount.load() == 0);
        assert(wrongStartCascadeCount.load() == 0);
        assert(wrongMetadataCount.load() == 0);

        // Verify dead port 59999 was pruned from agy_ls_endpoints.json while live wrongPort was preserved
        {
            std::ifstream ef(endpointsFile);
            json prunedEj = json::parse(ef);
            assert(prunedEj.contains("endpoints"));
            assert(prunedEj["endpoints"].contains(std::to_string(wrongPort)));
            assert(!prunedEj["endpoints"].contains("59999"));
        }

        // 6b. Register lockedPort's CSRF token via telemetry event and verify prompt pins strictly to lockedPort
        json evRegisterLs = {
            {"event_type", "LS_ENDPOINT_REGISTER"},
            {"session_id", lockedCid},
            {"conversation_id", lockedCid},
            {"agent_type", "antigravity"},
            {"workspace", "/tmp"},
            {"details", {
                {"ls_address", "127.0.0.1:" + std::to_string(lockedPort)},
                {"csrf_token", lockedCsrf}
            }},
            {"timestamp", static_cast<int64_t>(time(nullptr))}
        };
        auto resReg = client.Post("/api/telemetry/event", evRegisterLs.dump(), "application/json");
        assert(resReg && resReg->status == 200);

        {
            std::ifstream ef(endpointsFile);
            json regEj = json::parse(ef);
            assert(regEj["endpoints"].contains(std::to_string(lockedPort)));
            assert(regEj["endpoints"][std::to_string(lockedPort)].value("csrf_token", "") == lockedCsrf);
        }

        promptReq["prompt"] = "Hello active TUI session";
        auto resPinned = client.Post("/api/agent/prompt", mobileAuthHeaders, promptReq.dump(), "application/json");
        assert(resPinned && resPinned->status == 200);
        json pinnedJson = json::parse(resPinned->body);
        assert(pinnedJson.value("status", "") == "dispatched");
        assert(pinnedJson.value("conversationId", "") == lockedCid);
        assert(lockedSendCount.load() == 1);
        assert(wrongSendCount.load() == 0);
        assert(wrongMetadataCount.load() == 0);

        // 6c. Live in-memory GET /api/agent/transcript fallback must also pin to lockedPort
        auto resLiveTranscript = client.Get(("/api/agent/transcript?id=" + lockedCid).c_str(), mobileAuthHeaders);
        assert(resLiveTranscript && resLiveTranscript->status == 200);
        json liveTj = json::parse(resLiveTranscript->body);
        assert(liveTj.value("source", "") == "live");
        assert(liveTj.contains("steps") && liveTj["steps"].size() == 2);
        assert(liveTj["steps"][1].value("text", "") == "LOCKED_TUI_LIVE_STEP");
        assert(lockedTrajectoryCount.load() >= 1);

        unsetenv("AIMON_CLI_BRAIN_DIR");
        unsetenv("AIMON_IDE_BRAIN_DIR");
        unsetenv("AIMON_AGY_PROCESSES_FILE");
        unsetenv("AIMON_AGY_ENDPOINTS_FILE");
        unsetenv("AIMON_AGY_BIN");

        wrongLs.stop();
        lockedLs.stop();
        if (wrongThread.joinable()) wrongThread.join();
        if (lockedThread.joinable()) lockedThread.join();
        fs::remove_all(tmpDir);
    }

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
