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
#include <sstream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <sqlite3.h>
#include <sys/stat.h>
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
        fs::create_directories(tmpDir / "cli_brain" / "locked-conv-uuid-777");
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
        assert(resMissingTok && (resMissingTok->status == 409 || resMissingTok->status == 503));
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

    // 7. Cursor Pending store.db AskQuestion, Claude AskUserQuestion (multiSelect + toolUseResult.answers),
    //    and Provider-Aware TUI Keystroke Routing via Mock AIMON_SCREEN_BIN
    {
        namespace fs = std::filesystem;
        fs::path tmpDir = "/tmp/test_webserver_ask_question";
        fs::remove_all(tmpDir);

        fs::path cursorProjDir = tmpDir / "cursor_projects";
        fs::path cursorChatsDir = tmpDir / "cursor_chats";
        fs::path cursorHooksDir = tmpDir / "cursor_hooks";
        fs::path claudeProjDir = tmpDir / "claude_projects";
        fs::path claudeHooksDir = tmpDir / "claude_hooks";
        fs::create_directories(cursorProjDir);
        fs::create_directories(cursorChatsDir);
        fs::create_directories(cursorHooksDir);
        fs::create_directories(claudeProjDir);
        fs::create_directories(claudeHooksDir);

        setenv("AIMON_CURSOR_PROJECTS_DIR", cursorProjDir.c_str(), 1);
        setenv("AIMON_CURSOR_CHATS_DIR", cursorChatsDir.c_str(), 1);
        setenv("AIMON_CURSOR_HOOK_STATE_DIR", cursorHooksDir.c_str(), 1);
        setenv("AIMON_CLAUDE_PROJECTS_DIR", claudeProjDir.c_str(), 1);
        setenv("AIMON_CLAUDE_HOOK_STATE_DIR", claudeHooksDir.c_str(), 1);
        setenv("AIMON_LIVE_ROUTE_POLL_MS", "30", 1);
        setenv("AIMON_LIVE_ROUTE_TIMEOUT_MS", "4000", 1);

        auto hexEncode = [](const std::string& input) -> std::string {
            std::ostringstream oss;
            for (unsigned char c : input) {
                oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c);
            }
            return oss.str();
        };

        auto writeCursorStoreDb = [&](const fs::path& dbPath,
                                      const std::string& rootBlobId,
                                      const json& rootBlobJson) {
            fs::create_directories(dbPath.parent_path());
            fs::remove(dbPath);
            sqlite3* db = nullptr;
            assert(sqlite3_open(dbPath.c_str(), &db) == SQLITE_OK);
            char* errMsg = nullptr;
            assert(sqlite3_exec(db, "CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT);", nullptr, nullptr, &errMsg) == SQLITE_OK);
            assert(sqlite3_exec(db, "CREATE TABLE blobs (id TEXT PRIMARY KEY, data BLOB);", nullptr, nullptr, &errMsg) == SQLITE_OK);

            std::string metaJsonStr = json({{"agentId", "test"}, {"latestRootBlobId", rootBlobId}}).dump();
            std::string metaHex = hexEncode(metaJsonStr);
            std::string insMeta = "INSERT INTO meta (key, value) VALUES ('0', '" + metaHex + "');";
            assert(sqlite3_exec(db, insMeta.c_str(), nullptr, nullptr, &errMsg) == SQLITE_OK);

            std::string blobStr = rootBlobJson.dump();
            sqlite3_stmt* stmt = nullptr;
            assert(sqlite3_prepare_v2(db, "INSERT INTO blobs (id, data) VALUES (?, ?);", -1, &stmt, nullptr) == SQLITE_OK);
            sqlite3_bind_text(stmt, 1, rootBlobId.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_blob(stmt, 2, blobStr.data(), static_cast<int>(blobStr.size()), SQLITE_TRANSIENT);
            assert(sqlite3_step(stmt) == SQLITE_DONE);
            sqlite3_finalize(stmt);
            sqlite3_close(db);
        };

        // 7a. Cursor pending store.db AskQuestion extraction & normalization
        std::string cursorCid = "cursor-askq-uuid-101";
        fs::path cursorTransFile = cursorProjDir / "workspace-aimon" / "agent-transcripts" /
                                   cursorCid / (cursorCid + ".jsonl");
        fs::create_directories(cursorTransFile.parent_path());
        {
            std::ofstream f(cursorTransFile);
            f << json({
                {"role", "user"},
                {"message", {{"content", json::array({{{"type", "text"}, {"text", "<user_query>\nAsk me a question\n</user_query>"}}})}}}
            }).dump() << "\n";
        }

        fs::path cursorDbPath = cursorChatsDir / "ws-hash-001" / cursorCid / "store.db";
        json cursorPendingBlob = {
            {"role", "assistant"},
            {"content", json::array({
                {{"type", "reasoning"}, {"text", "Thinking about options..."}},
                {{"type", "text"}, {"text", "Please choose one of the options below:"}},
                {
                    {"type", "tool-call"},
                    {"toolCallId", "toolu_cursor_ask_1"},
                    {"toolName", "AskQuestion"},
                    {"args", {
                        {"title", "Target Selection"},
                        {"questions", json::array({
                            {
                                {"id", "q_target"},
                                {"prompt", "Which target environment should we use?"},
                                {"allowMultiple", false},
                                {"options", json::array({
                                    {{"id", "opt_a"}, {"label", "Alpha"}},
                                    {{"id", "opt_b"}, {"label", "Beta"}},
                                    {{"id", "opt_c"}, {"label", "Gamma"}}
                                })}
                            }
                        })}
                    }}
                }
            })},
            {"providerOptions", {
                {"cursor", {
                    {"pendingToolCallStartedAtMs", 1775948534000ULL},
                    {"pendingToolExecutionContracts", json::array({
                        {
                            {"toolCallId", "toolu_cursor_ask_1"},
                            {"toolIdentifier", "ASK_QUESTION"},
                            {"outerToolName", "AskQuestion"}
                        }
                    })}
                }}
            }}
        };
        writeCursorStoreDb(cursorDbPath, "blob-root-101", cursorPendingBlob);

        auto resCurTrans = client.Get(("/api/agent/transcript?id=" + cursorCid).c_str(), mobileAuthHeaders);
        assert(resCurTrans && resCurTrans->status == 200);
        json curTj = json::parse(resCurTrans->body);
        assert(curTj.value("source", "") == "cursor");
        assert(curTj.value("generating", true) == false);
        assert(curTj.contains("steps") && curTj["steps"].size() == 2);
        const auto& curLastStep = curTj["steps"].back();
        assert(curLastStep.value("type", "") == "PLANNER_RESPONSE");
        assert(curLastStep.value("thinking", "") == "Thinking about options...");
        assert(curLastStep.value("text", "") == "Please choose one of the options below:");
        assert(curLastStep.contains("askQuestion") && curLastStep["askQuestion"].is_object());
        assert(curLastStep["askQuestion"].value("answered", true) == false);
        const auto& curQs = curLastStep["askQuestion"]["questions"];
        assert(curQs.is_array() && curQs.size() == 1);
        assert(curQs[0].value("id", "") == "q_target");
        assert(curQs[0].value("question", "") == "Which target environment should we use?");
        assert(curQs[0].value("isMultiSelect", true) == false);
        assert(curQs[0]["options"] == json::array({"Alpha", "Beta", "Gamma"}));
        assert(curQs[0]["optionObjects"].size() == 3);
        assert(curQs[0]["optionObjects"][0].value("id", "") == "opt_a");
        assert(curQs[0]["optionObjects"][0].value("label", "") == "Alpha");
        assert(curQs[0]["optionObjects"][1].value("id", "") == "opt_b");
        assert(curQs[0]["optionObjects"][1].value("label", "") == "Beta");

        // 7b. Fault cases: corrupt store.db and already-answered toolCallId in .jsonl
        std::string corruptCid = "cursor-corrupt-uuid-102";
        fs::path corruptTransFile = cursorProjDir / "workspace-aimon" / "agent-transcripts" /
                                    corruptCid / (corruptCid + ".jsonl");
        fs::create_directories(corruptTransFile.parent_path());
        {
            std::ofstream f(corruptTransFile);
            f << json({
                {"role", "user"},
                {"message", {{"content", "Test corrupt store.db"}}}
            }).dump() << "\n";
        }
        fs::path corruptDbPath = cursorChatsDir / "ws-hash-001" / corruptCid / "store.db";
        fs::create_directories(corruptDbPath.parent_path());
        {
            std::ofstream badDb(corruptDbPath, std::ios::binary);
            badDb << "NOT_A_VALID_SQLITE_DATABASE_HEADER_BYTES";
        }
        auto resCorrupt = client.Get(("/api/agent/transcript?id=" + corruptCid).c_str(), mobileAuthHeaders);
        assert(resCorrupt && resCorrupt->status == 200);
        json corruptTj = json::parse(resCorrupt->body);
        assert(corruptTj["steps"].size() == 1);
        assert(!corruptTj["steps"].back().contains("askQuestion"));

        // 7c. Claude AskUserQuestion with multiSelect: true and toolUseResult.answers
        std::string claudeCid = "claude-askq-uuid-201";
        fs::path claudeTransFile = claudeProjDir / "-workspace-aimon" / (claudeCid + ".jsonl");
        fs::create_directories(claudeTransFile.parent_path());
        {
            std::ofstream f(claudeTransFile);
            f << json({
                {"type", "user"},
                {"message", {{"role", "user"}, {"content", "Ask me a multi-select question"}}}
            }).dump() << "\n";
            f << json({
                {"type", "assistant"},
                {"message", {
                    {"role", "assistant"},
                    {"model", "claude-sonnet-4-6"},
                    {"stop_reason", "tool_use"},
                    {"content", json::array({
                        {{"type", "text"}, {"text", "Select the features to enable:"}},
                        {
                            {"type", "tool_use"},
                            {"id", "toolu_claude_ask_1"},
                            {"name", "AskUserQuestion"},
                            {"input", {
                                {"questions", json::array({
                                    {
                                        {"question", "Which features should we enable?"},
                                        {"header", "Features"},
                                        {"multiSelect", true},
                                        {"options", json::array({
                                            {{"label", "Choice 1"}, {"description", "First capability"}},
                                            {{"label", "Choice 2"}, {"description", "Second capability"}}
                                        })}
                                    }
                                })}
                            }}
                        }
                    })}
                }}
            }).dump() << "\n";
        }

        auto resClaudePending = client.Get(("/api/agent/transcript?id=" + claudeCid).c_str(), mobileAuthHeaders);
        assert(resClaudePending && resClaudePending->status == 200);
        json claudePendingTj = json::parse(resClaudePending->body);
        assert(claudePendingTj.value("source", "") == "claude");
        assert(claudePendingTj.value("generating", true) == false);
        assert(claudePendingTj["steps"].size() == 2);
        const auto& claudeStep = claudePendingTj["steps"].back();
        assert(claudeStep.contains("askQuestion") && claudeStep["askQuestion"].is_object());
        assert(claudeStep["askQuestion"].value("answered", true) == false);
        const auto& claudeQs = claudeStep["askQuestion"]["questions"];
        assert(claudeQs.size() == 1);
        assert(claudeQs[0].value("question", "") == "Which features should we enable?");
        assert(claudeQs[0].value("header", "") == "Features");
        assert(claudeQs[0].value("isMultiSelect", false) == true);
        assert(claudeQs[0]["options"] == json::array({"Choice 1", "Choice 2"}));
        assert(claudeQs[0]["optionObjects"][0].value("description", "") == "First capability");
        assert(claudeQs[0]["optionObjects"][1].value("description", "") == "Second capability");

        // Append user tool_result with toolUseResult.answers and verify answered == true & answers populated
        {
            std::ofstream f(claudeTransFile, std::ios::app);
            f << json({
                {"type", "user"},
                {"message", {
                    {"role", "user"},
                    {"content", json::array({
                        {
                            {"type", "tool_result"},
                            {"tool_use_id", "toolu_claude_ask_1"},
                            {"content", "User answered AskUserQuestion"}
                        }
                    })}
                }},
                {"toolUseResult", {
                    {"answers", {
                        {"Which features should we enable?", "Choice 2"}
                    }}
                }}
            }).dump() << "\n";
            f << json({
                {"type", "assistant"},
                {"message", {
                    {"role", "assistant"},
                    {"model", "claude-sonnet-4-6"},
                    {"stop_reason", "end_turn"},
                    {"content", json::array({
                        {{"type", "text"}, {"text", "Enabled Choice 2."}}
                    })}
                }}
            }).dump() << "\n";
        }

        auto resClaudeAnswered = client.Get(("/api/agent/transcript?id=" + claudeCid).c_str(), mobileAuthHeaders);
        assert(resClaudeAnswered && resClaudeAnswered->status == 200);
        json claudeAnsTj = json::parse(resClaudeAnswered->body);
        assert(claudeAnsTj["steps"].size() == 3);
        const auto& answeredQStep = claudeAnsTj["steps"][1];
        assert(answeredQStep["askQuestion"].value("answered", false) == true);
        assert(answeredQStep["askQuestion"]["answers"] == json::array({"Choice 2"}));

        // 7d. TUI Keystroke Synthesis & Multiplexer Routing via Mock AIMON_SCREEN_BIN
        fs::path keyLogFile = tmpDir / "screen_keys_hex.log";
        fs::path mockScreenScript = tmpDir / "mock_screen.sh";
        {
            std::ofstream ms(mockScreenScript);
            ms << "#!/usr/bin/env bash\n"
               << "last_arg=\"${!#}\"\n"
               << "hex_val=$(printf '%s' \"$last_arg\" | od -An -tx1 | tr -d ' \\n')\n"
               << "echo \"$hex_val\" >> \"" << keyLogFile.string() << "\"\n"
               << "if [ -n \"$MOCK_CURSOR_TRANSCRIPT\" ] && { [ \"$hex_val\" = \"0d\" ] || [ \"$hex_val\" = \"73\" ]; }; then\n"
               << "  echo '{\"role\":\"assistant\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"Cursor received TUI selection.\"}]}}' >> \"$MOCK_CURSOR_TRANSCRIPT\"\n"
               << "  echo '{\"type\":\"turn_ended\",\"status\":\"success\"}' >> \"$MOCK_CURSOR_TRANSCRIPT\"\n"
               << "fi\n"
               << "if [ -n \"$MOCK_CLAUDE_TRANSCRIPT\" ] && [ \"$hex_val\" = \"0d\" ]; then\n"
               << "  cr_count=$(grep -c '^0d$' \"" << keyLogFile.string() << "\" || true)\n"
               << "  if [ \"$cr_count\" -ge 2 ]; then\n"
               << "    echo '{\"type\":\"assistant\",\"message\":{\"role\":\"assistant\",\"stop_reason\":\"end_turn\",\"content\":[{\"type\":\"text\",\"text\":\"Claude received TUI selection.\"}]}}' >> \"$MOCK_CLAUDE_TRANSCRIPT\"\n"
               << "  fi\n"
               << "fi\n"
               << "exit 0\n";
        }
        chmod(mockScreenScript.c_str(), 0755);

        setenv("AIMON_SCREEN_BIN", mockScreenScript.c_str(), 1);
        setenv("AIMON_SCREEN_SESSION_OVERRIDE", "12345.mock_pts", 1);

        auto readHexLog = [&]() -> std::vector<std::string> {
            std::vector<std::string> out;
            std::ifstream lf(keyLogFile);
            std::string l;
            while (std::getline(lf, l)) {
                if (!l.empty()) out.push_back(l);
            }
            return out;
        };

        // Case 1: Cursor single-select ("Beta" -> index 1 -> 1x Down-arrow "1b5b42", Enter "0d")
        fs::remove(keyLogFile);
        setenv("MOCK_CURSOR_TRANSCRIPT", cursorTransFile.c_str(), 1);
        unsetenv("MOCK_CLAUDE_TRANSCRIPT");
        json reqCurSingle = {
            {"agentId", "cursor"},
            {"source", "cursor"},
            {"conversationId", cursorCid},
            {"prompt", "Q1: Which target environment should we use?\nA1: Beta"},
            {"questionResponses", json::array({
                {
                    {"question", "Which target environment should we use?"},
                    {"selectedOptions", json::array({"Beta"})},
                    {"writeInResponse", ""}
                }
            })}
        };
        auto resCurSingle = client.Post("/api/agent/prompt", mobileAuthHeaders, reqCurSingle.dump(), "application/json");
        assert(resCurSingle && resCurSingle->status == 200);
        json curSingleResp = json::parse(resCurSingle->body);
        assert(curSingleResp.value("status", "") == "ok");
        assert(curSingleResp.value("response", "").find("Cursor received TUI selection.") != std::string::npos);
        std::vector<std::string> curSingleKeys = readHexLog();
        assert((curSingleKeys == std::vector<std::string>{"1b5b42", "0d"}));

        // Case 2: Cursor write-in ("Custom env" on 3 options -> 3x Down-arrow "1b5b42", "Custom env", Enter "0d")
        std::string cursorWriteInCid = "cursor-askq-writein-103";
        fs::path cursorWriteInTrans = cursorProjDir / "workspace-aimon" / "agent-transcripts" /
                                      cursorWriteInCid / (cursorWriteInCid + ".jsonl");
        fs::create_directories(cursorWriteInTrans.parent_path());
        {
            std::ofstream f(cursorWriteInTrans);
            f << json({
                {"role", "user"},
                {"message", {{"content", "Ask me write-in"}}}
            }).dump() << "\n";
        }
        writeCursorStoreDb(cursorChatsDir / "ws-hash-001" / cursorWriteInCid / "store.db",
                           "blob-root-103", cursorPendingBlob);
        fs::remove(keyLogFile);
        setenv("MOCK_CURSOR_TRANSCRIPT", cursorWriteInTrans.c_str(), 1);
        json reqCurWriteIn = {
            {"agentId", "cursor"},
            {"source", "cursor"},
            {"conversationId", cursorWriteInCid},
            {"prompt", "Custom env"},
            {"questionResponses", json::array({
                {
                    {"question", "Which target environment should we use?"},
                    {"selectedOptions", json::array()},
                    {"writeInResponse", "Custom env"}
                }
            })}
        };
        auto resCurWriteIn = client.Post("/api/agent/prompt", mobileAuthHeaders, reqCurWriteIn.dump(), "application/json");
        assert(resCurWriteIn && resCurWriteIn->status == 200);
        std::vector<std::string> curWriteInKeys = readHexLog();
        assert((curWriteInKeys == std::vector<std::string>{
            "1b5b42", "1b5b42", "1b5b42", hexEncode("Custom env"), "0d"
        }));

        // Case 3: Cursor multi-select ("Alpha" [0] & "Gamma" [2] -> Space "20", 2x Down "1b5b42", Space "20", "s" "73")
        std::string cursorMultiCid = "cursor-askq-multi-104";
        fs::path cursorMultiTrans = cursorProjDir / "workspace-aimon" / "agent-transcripts" /
                                    cursorMultiCid / (cursorMultiCid + ".jsonl");
        fs::create_directories(cursorMultiTrans.parent_path());
        {
            std::ofstream f(cursorMultiTrans);
            f << json({
                {"role", "user"},
                {"message", {{"content", "Ask me multi-select"}}}
            }).dump() << "\n";
        }
        json cursorMultiBlob = cursorPendingBlob;
        cursorMultiBlob["content"][2]["args"]["questions"][0]["allowMultiple"] = true;
        writeCursorStoreDb(cursorChatsDir / "ws-hash-001" / cursorMultiCid / "store.db",
                           "blob-root-104", cursorMultiBlob);
        fs::remove(keyLogFile);
        setenv("MOCK_CURSOR_TRANSCRIPT", cursorMultiTrans.c_str(), 1);
        json reqCurMulti = {
            {"agentId", "cursor"},
            {"source", "cursor"},
            {"conversationId", cursorMultiCid},
            {"prompt", "Alpha, Gamma"},
            {"questionResponses", json::array({
                {
                    {"question", "Which target environment should we use?"},
                    {"selectedOptions", json::array({"Alpha", "Gamma"})},
                    {"writeInResponse", ""}
                }
            })}
        };
        auto resCurMulti = client.Post("/api/agent/prompt", mobileAuthHeaders, reqCurMulti.dump(), "application/json");
        assert(resCurMulti && resCurMulti->status == 200);
        std::vector<std::string> curMultiKeys = readHexLog();
        assert((curMultiKeys == std::vector<std::string>{
            "20", "1b5b42", "1b5b42", "20", "73"
        }));

        // Case 4: Claude multi-select ("Choice 1" [0] & "Choice 2" [1] on 2 options ->
        //         Space "20", Down "1b5b42", Space "20", 2x Down to Submit [index 3], Enter "0d", Review Enter "0d")
        std::string claudeLiveCid = "claude-askq-live-202";
        fs::path claudeLiveTrans = claudeProjDir / "-workspace-aimon" / (claudeLiveCid + ".jsonl");
        {
            std::ofstream f(claudeLiveTrans);
            f << json({
                {"type", "user"},
                {"message", {{"role", "user"}, {"content", "Ask me live multi-select"}}}
            }).dump() << "\n";
            f << json({
                {"type", "assistant"},
                {"message", {
                    {"role", "assistant"},
                    {"model", "claude-sonnet-4-6"},
                    {"stop_reason", "tool_use"},
                    {"content", json::array({
                        {
                            {"type", "tool_use"},
                            {"id", "toolu_claude_live_1"},
                            {"name", "AskUserQuestion"},
                            {"input", {
                                {"questions", json::array({
                                    {
                                        {"question", "Which features should we enable?"},
                                        {"multiSelect", true},
                                        {"options", json::array({
                                            {{"label", "Choice 1"}},
                                            {{"label", "Choice 2"}}
                                        })}
                                    }
                                })}
                            }}
                        }
                    })}
                }}
            }).dump() << "\n";
        }
        fs::remove(keyLogFile);
        unsetenv("MOCK_CURSOR_TRANSCRIPT");
        setenv("MOCK_CLAUDE_TRANSCRIPT", claudeLiveTrans.c_str(), 1);
        json reqClaudeMulti = {
            {"agentId", "claude"},
            {"source", "claude"},
            {"conversationId", claudeLiveCid},
            {"prompt", "Choice 1, Choice 2"},
            {"questionResponses", json::array({
                {
                    {"question", "Which features should we enable?"},
                    {"selectedOptions", json::array({"Choice 1", "Choice 2"})},
                    {"writeInResponse", ""}
                }
            })}
        };
        auto resClaudeMulti = client.Post("/api/agent/prompt", mobileAuthHeaders, reqClaudeMulti.dump(), "application/json");
        assert(resClaudeMulti && resClaudeMulti->status == 200);
        std::vector<std::string> claudeMultiKeys = readHexLog();
        assert((claudeMultiKeys == std::vector<std::string>{
            "20", "1b5b42", "20", "1b5b42", "1b5b42", "0d", "0d"
        }));

        unsetenv("MOCK_CURSOR_TRANSCRIPT");
        unsetenv("MOCK_CLAUDE_TRANSCRIPT");
        unsetenv("AIMON_SCREEN_BIN");
        unsetenv("AIMON_SCREEN_SESSION_OVERRIDE");
        unsetenv("AIMON_CURSOR_PROJECTS_DIR");
        unsetenv("AIMON_CURSOR_CHATS_DIR");
        unsetenv("AIMON_CURSOR_HOOK_STATE_DIR");
        unsetenv("AIMON_CLAUDE_PROJECTS_DIR");
        unsetenv("AIMON_CLAUDE_HOOK_STATE_DIR");
        unsetenv("AIMON_LIVE_ROUTE_POLL_MS");
        unsetenv("AIMON_LIVE_ROUTE_TIMEOUT_MS");
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
