/*
 * TestMobileGateway.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <cassert>
#include <iostream>
#include <thread>
#include <chrono>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>
#include <rapidcheck.h>
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include <httplib.h>
#include "MobileGateway.hxx"
#include "AgentTelemetryDb.hxx"
#include "WebServer.hxx"
#include "StateStore.hxx"
#include "HistoryStore.hxx"

namespace fs = std::filesystem;
using namespace aimon;
using json = nlohmann::json;

static void testPairingAndAuth() {
    std::cout << "[TestMobileGateway] Running testPairingAndAuth..." << std::endl;
    auto &gw = MobileGateway::getInstance();
    std::string tmpPath = "/tmp/aimon_test_pairing_" + std::to_string(getpid()) + ".json";
    fs::remove(tmpPath);
    assert(gw.init(tmpPath));

    // 1. Create pairing secret and verify 30s reuse cache + <=0 default duration
    std::string secretShort = gw.createPairingSecret(10);
    assert(secretShort.length() == 32);
    // Since secretShort expires in 10s (< 30s reuse threshold), next call generates a fresh secret
    std::string secret = gw.createPairingSecret(0);
    assert(!secret.empty());
    assert(secret.length() == 32); // 16 bytes hex
    assert(secret != secretShort);
    assert(gw.createPairingSecret(60) == secret);

    // 2. Reject incorrect secret
    std::string token;
    assert(!gw.pairDevice("wrongsecret1234567890123456789012", "dev-1", "My Phone", token));
    assert(token.empty());

    // 3. Pair device with correct secret
    assert(gw.pairDevice(secret, "dev-1", "Pixel 9 Pro", token));
    assert(!token.empty());
    assert(token.length() == 64); // 32 bytes hex

    // 4. Pairing secret should be consumed (single-use)
    std::string token2;
    assert(!gw.pairDevice(secret, "dev-2", "Pixel 9 Pro 2", token2));

    // 5. Authenticate with valid token
    std::string outDevId;
    assert(gw.authenticate(token, outDevId));
    assert(outDevId == "dev-1");

    // 6. Reject invalid token
    std::string badDevId;
    assert(!gw.authenticate("badtoken123456", badDevId));

    // 7. Device listing
    auto devs = gw.listDevices();
    assert(devs.size() == 1);
    assert(devs[0].deviceId == "dev-1");
    assert(devs[0].deviceName == "Pixel 9 Pro");

    // 8. Revoke device
    gw.revokeDevice("dev-1");
    assert(!gw.authenticate(token, outDevId));

    fs::remove(tmpPath);
    std::cout << "[TestMobileGateway] testPairingAndAuth passed!" << std::endl;
}

static void testSessionPersistenceAndPermissions() {
    std::cout << "[TestMobileGateway] Running testSessionPersistenceAndPermissions..." << std::endl;
    std::string tempDir = "/tmp/aimon_mobile_gw_persist_" + std::to_string(getpid());
    std::string tempFile = tempDir + "/mobile_devices.json";
    fs::remove_all(tempDir);

    std::string tokenA;
    std::string tokenB;
    {
        MobileGateway gw1;
        assert(gw1.init(tempFile));

        std::string secretA = gw1.createPairingSecret(120);
        assert(gw1.verifyPairingCode(secretA, "dev-alpha", "Pixel 9 Pro", tokenA));
        assert(!tokenA.empty());

        std::string secretB = gw1.createPairingSecret(120);
        assert(gw1.verifyPairingCode(secretB, "dev-beta", "Galaxy S25", tokenB));
        assert(!tokenB.empty());

        struct stat st;
        assert(stat(tempFile.c_str(), &st) == 0);
        assert((st.st_mode & 0777) == 0600);

        nlohmann::json toolArgs = {{"CommandLine", "echo test"}};
        std::string appId = gw1.submitApprovalRequest("antigravity", "run_command", "/workspace/aimon", toolArgs, "Verify remaining_seconds", 90);
        auto pending = gw1.getPendingApprovals();
        assert(pending.size() == 1);
        int rem = pending[0].value("remaining_seconds", 0);
        assert(rem > 0 && rem <= 90);
        assert(gw1.resolveApproval(appId, ApprovalVerdict::APPROVED));
        assert(gw1.waitForApproval(appId, 2) == ApprovalVerdict::APPROVED);
    }

    // Second instance restores paired sessions from tempFile
    {
        MobileGateway gw2;
        assert(gw2.init(tempFile));
        auto devs = gw2.getPairedDevices();
        assert(devs.size() == 2);
        assert(gw2.validateToken(tokenA));
        assert(gw2.validateToken(tokenB));

        gw2.revokeDevice("dev-alpha");
        assert(!gw2.validateToken(tokenA));
        assert(gw2.validateToken(tokenB));

        struct stat st;
        assert(stat(tempFile.c_str(), &st) == 0);
        assert((st.st_mode & 0777) == 0600);
    }

    // Third instance confirms revocation persisted on disk
    {
        MobileGateway gw3;
        assert(gw3.init(tempFile));
        auto devs = gw3.getPairedDevices();
        assert(devs.size() == 1);
        assert(devs[0].deviceId == "dev-beta");
        assert(!gw3.validateToken(tokenA));
        assert(gw3.validateToken(tokenB));
    }

    fs::remove_all(tempDir);
    std::cout << "[TestMobileGateway] testSessionPersistenceAndPermissions passed!" << std::endl;
}

static void testRapidCheckTokenValidation() {
    std::cout << "[TestMobileGateway] Running testRapidCheckTokenValidation..." << std::endl;
    std::string tempFile = "/tmp/aimon_rc_mobile_" + std::to_string(getpid()) + ".json";
    fs::remove(tempFile);

    MobileGateway gw;
    assert(gw.init(tempFile));

    std::string secret = gw.createPairingSecret(300);
    std::string validToken;
    assert(gw.verifyPairingCode(secret, "rc-dev-1", "RC Phone", validToken));
    assert(!validToken.empty());

    bool ok = rc::check("arbitrary invalid tokens are rejected while paired token succeeds until revoked",
                        [&]() {
        const auto candidate = *rc::gen::container<std::string>(rc::gen::arbitrary<char>());
        if (candidate != validToken) {
            RC_ASSERT(!gw.validateToken(candidate));
        }
        RC_ASSERT(gw.validateToken(validToken));
    });
    assert(ok);

    gw.revokeDevice("rc-dev-1");

    bool okAfterRevoke = rc::check("all tokens including revoked token are rejected after revokeDevice",
                                   [&]() {
        const auto candidate = *rc::gen::container<std::string>(rc::gen::arbitrary<char>());
        RC_ASSERT(!gw.validateToken(candidate));
        RC_ASSERT(!gw.validateToken(validToken));
    });
    assert(okAfterRevoke);

    fs::remove(tempFile);
    std::cout << "[TestMobileGateway] testRapidCheckTokenValidation passed!" << std::endl;
}

static void testApprovalLatching() {
    std::cout << "[TestMobileGateway] Running testApprovalLatching..." << std::endl;
    auto &gw = MobileGateway::getInstance();

    std::string lastBroadcastType;
    nlohmann::json lastBroadcastPayload;
    gw.setBroadcastCallback([&](const std::string &type, const nlohmann::json &payload) {
        lastBroadcastType = type;
        lastBroadcastPayload = payload;
    });

    nlohmann::json toolArgs;
    toolArgs["CommandLine"] = "make -j8";

    std::string approvalId = gw.submitApprovalRequest("antigravity", "run_command", "/workspace/aimon", toolArgs, "Build target", 5);
    assert(!approvalId.empty());
    assert(lastBroadcastType == "approval_request");
    assert(lastBroadcastPayload.value("timeout_seconds", 0) == 5);

    // Check default timeout fallback when timeoutSeconds <= 0
    std::string defaultTimeoutId = gw.submitApprovalRequest("antigravity", "run_command", "/workspace/aimon", toolArgs, "Default timeout", 0);
    auto stDefault = gw.getApprovalStatus(defaultTimeoutId);
    assert(stDefault.value("status", "") == "pending");
    assert(stDefault.value("verdict", "") == "PENDING");
    assert(stDefault.value("remaining_seconds", 0) > 60 && stDefault.value("remaining_seconds", 0) <= 120);
    assert(gw.resolveApproval(defaultTimeoutId, ApprovalVerdict::DENIED));
    assert(lastBroadcastType == "approval_resolved");
    assert(lastBroadcastPayload.value("verdict", "") == "DENIED");
    auto stDeniedPending = gw.getApprovalStatus(defaultTimeoutId);
    assert(stDeniedPending.value("status", "") == "denied");
    assert(stDeniedPending.value("verdict", "") == "DENIED");
    assert(gw.waitForApproval(defaultTimeoutId, 0) == ApprovalVerdict::DENIED);
    auto stDeniedResolved = gw.getApprovalStatus(defaultTimeoutId);
    assert(stDeniedResolved.value("status", "") == "denied");
    assert(stDeniedResolved.value("verdict", "") == "DENIED");

    // Check pending list
    auto pending = gw.listPendingApprovals();
    assert(pending.size() == 1);
    assert(pending[0]["approval_id"] == approvalId);
    assert(pending[0]["tool_name"] == "run_command");
    assert(pending[0].value("remaining_seconds", 0) > 0 && pending[0].value("remaining_seconds", 0) <= 5);

    auto stPending = gw.getApprovalStatus(approvalId);
    assert(stPending.value("status", "") == "pending");
    assert(stPending.value("verdict", "") == "PENDING");
    assert(stPending.value("remaining_seconds", 0) > 0 && stPending.value("remaining_seconds", 0) <= 5);

    // Spawn background thread to simulate user approving from Android app
    std::thread approverThread([&gw, approvalId]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        assert(gw.resolveApproval(approvalId, ApprovalVerdict::APPROVED));
    });

    // Main thread waits on approval
    ApprovalVerdict verdict = gw.waitForApproval(approvalId, 5);
    assert(verdict == ApprovalVerdict::APPROVED);

    approverThread.join();
    assert(lastBroadcastType == "approval_resolved");
    assert(lastBroadcastPayload.value("verdict", "") == "APPROVED");

    auto stApprovedResolved = gw.getApprovalStatus(approvalId);
    assert(stApprovedResolved.value("status", "") == "approved");
    assert(stApprovedResolved.value("verdict", "") == "APPROVED");

    // Also test getApprovalStatus when resolved before waitForApproval erases from _pendingApprovals
    std::string preResolveId = gw.submitApprovalRequest("antigravity", "run_command", "/workspace/aimon", toolArgs, "Pre-resolve", 5);
    assert(gw.resolveApproval(preResolveId, ApprovalVerdict::APPROVED));
    auto stApprovedPending = gw.getApprovalStatus(preResolveId);
    assert(stApprovedPending.value("status", "") == "approved");
    assert(stApprovedPending.value("verdict", "") == "APPROVED");
    assert(gw.waitForApproval(preResolveId, 5) == ApprovalVerdict::APPROVED);

    // Re-resolving resolved approval should fail
    assert(!gw.resolveApproval(approvalId, ApprovalVerdict::APPROVED));
    gw.setBroadcastCallback(nullptr);

    std::cout << "[TestMobileGateway] testApprovalLatching passed!" << std::endl;
}

static void testApprovalTimeout() {
    std::cout << "[TestMobileGateway] Running testApprovalTimeout..." << std::endl;
    auto &gw = MobileGateway::getInstance();

    nlohmann::json toolArgs;
    toolArgs["TargetFile"] = "/tmp/sensitive.conf";

    std::string approvalId = gw.submitApprovalRequest("cursor", "write_to_file", "/workspace/aimon", toolArgs, "Write config", 1);
    assert(!approvalId.empty());

    // Wait with 1-second timeout (no one resolves)
    ApprovalVerdict verdict = gw.waitForApproval(approvalId, 1);
    assert(verdict == ApprovalVerdict::TIMED_OUT);

    auto stExpiredResolved = gw.getApprovalStatus(approvalId);
    assert(stExpiredResolved.value("status", "") == "expired");
    assert(stExpiredResolved.value("verdict", "") == "TIMED_OUT");

    // Also verify expired entry still in _pendingApprovals before waitForApproval is called
    std::string expirePendingId = gw.submitApprovalRequest("cursor", "write_to_file", "/workspace/aimon", toolArgs, "Expire pending", 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    assert(gw.listPendingApprovals().empty());
    auto stExpiredPending = gw.getApprovalStatus(expirePendingId);
    assert(stExpiredPending.value("status", "") == "expired");
    assert(stExpiredPending.value("verdict", "") == "TIMED_OUT");
    assert(gw.waitForApproval(expirePendingId, 1) == ApprovalVerdict::TIMED_OUT);

    std::cout << "[TestMobileGateway] testApprovalTimeout passed!" << std::endl;
}

static void testWebServerMobileAuth() {
    std::cout << "[TestMobileGateway] Running testWebServerMobileAuth..." << std::endl;
    std::string tempFile = "/tmp/aimon_ws_mobile_" + std::to_string(getpid()) + ".json";
    fs::remove(tempFile);
    assert(MobileGateway::getInstance().init(tempFile));

    StateStore &stateStore = StateStore::getInstance();
    HistoryStore historyStore;
    WebConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.endpointsEnabled = false;

    WebServer server(stateStore, historyStore, config);
    assert(server.start(true));
    int port = server.getPort();
    assert(port > 0);

    httplib::Client client("http://127.0.0.1:" + std::to_string(port));
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(5, 0);

    // 1. Unauthenticated requests with User-Agent: aimon-mobile MUST be rejected with 401
    httplib::Headers spoofHeaders = {
        {"User-Agent", "aimon-mobile/2.0"}
    };
    auto resUnauthConv = client.Get("/api/agent/conversations", spoofHeaders);
    assert(resUnauthConv && resUnauthConv->status == 401);

    auto resUnauthPending = client.Get("/api/approvals/pending", spoofHeaders);
    assert(resUnauthPending && resUnauthPending->status == 401);

    auto resUnauthDevs = client.Get("/api/mobile/devices", spoofHeaders);
    assert(resUnauthDevs && resUnauthDevs->status == 401);

    auto resUnauthTree = client.Get("/api/workspace/tree", spoofHeaders);
    assert(resUnauthTree && resUnauthTree->status == 401);

    // 2. Invalid X-Mobile-Token MUST be rejected with 401
    httplib::Headers badTokenHeaders = {
        {"User-Agent", "aimon-mobile/2.0"},
        {"X-Mobile-Token", "invalid-token-value"}
    };
    auto resBadToken = client.Get("/api/approvals/pending", badTokenHeaders);
    assert(resBadToken && resBadToken->status == 401);

    // 3. Pairing via /api/mobile/qr (loopback) + /api/mobile/pair (no prior token) succeeds
    auto resQr = client.Get("/api/mobile/qr");
    assert(resQr && resQr->status == 200);
    json qrJson = json::parse(resQr->body);
    std::string secret = qrJson.value("secret", "");
    assert(!secret.empty());

    json pairReq = {
        {"secret", secret},
        {"device_id", "ws-auth-dev"},
        {"device_name", "Test Android"}
    };
    auto resPair = client.Post("/api/mobile/pair", pairReq.dump(), "application/json");
    assert(resPair && resPair->status == 200);
    json pairJson = json::parse(resPair->body);
    std::string validToken = pairJson.value("token", "");
    assert(!validToken.empty());

    // 4. Requests with valid X-Mobile-Token return 200
    httplib::Headers validHeaders = {
        {"User-Agent", "aimon-mobile/2.0"},
        {"X-Mobile-Token", validToken}
    };
    auto resAuthConv = client.Get("/api/agent/conversations", validHeaders);
    assert(resAuthConv && resAuthConv->status == 200);

    auto resAuthPending = client.Get("/api/approvals/pending", validHeaders);
    assert(resAuthPending && resAuthPending->status == 200);

    auto resAuthDevs = client.Get("/api/mobile/devices", validHeaders);
    assert(resAuthDevs && resAuthDevs->status == 200);

    // 5. Authorization: Bearer <token> also succeeds
    httplib::Headers bearerHeaders = {
        {"Authorization", "Bearer " + validToken}
    };
    auto resBearerPending = client.Get("/api/approvals/pending", bearerHeaders);
    assert(resBearerPending && resBearerPending->status == 200);

    // 6. Revoking the device invalidates its token immediately
    json revokeReq = {{"device_id", "ws-auth-dev"}};
    auto resRevoke = client.Post("/api/mobile/revoke", validHeaders, revokeReq.dump(), "application/json");
    assert(resRevoke && resRevoke->status == 200);

    auto resAfterRevoke = client.Get("/api/approvals/pending", validHeaders);
    assert(resAfterRevoke && resAfterRevoke->status == 401);

    server.stop();
    fs::remove(tempFile);
    std::cout << "[TestMobileGateway] testWebServerMobileAuth passed!" << std::endl;
}

static void testMultiRootBrainConversationDiscovery() {
    std::cout << "[TestMobileGateway] Running testMultiRootBrainConversationDiscovery..." << std::endl;
    std::string baseDir = "/tmp/aimon_env2_brains_" + std::to_string(getpid());
    std::string ideBrain = baseDir + "/ide_brain";
    std::string cliBrain = baseDir + "/cli_brain";
    std::string presenceDir = baseDir + "/presence";
    std::string tempMobileDb = baseDir + "/mobile_devices.json";
    fs::remove_all(baseDir);
    fs::create_directories(ideBrain);
    fs::create_directories(cliBrain);
    fs::create_directories(presenceDir);

    setenv("AIMON_IDE_BRAIN_DIR", ideBrain.c_str(), 1);
    setenv("AIMON_CLI_BRAIN_DIR", cliBrain.c_str(), 1);
    setenv("AIMON_CLI_PRESENCE_DIR", presenceDir.c_str(), 1);

    assert(MobileGateway::getInstance().init(tempMobileDb));
    std::string secret = MobileGateway::getInstance().createPairingSecret(120);
    std::string validToken;
    assert(MobileGateway::getInstance().verifyPairingCode(secret, "env2-dev", "Test Device", validToken));

    // Create 40 conversation directories across both IDE (22) and CLI (18) brain roots
    // Ensure timestamps vary widely so that premature 30-entry cutoff before sorting fails
    time_t baseTime = 1700000000;
    for (int i = 0; i < 40; ++i) {
        bool isCli = (i % 2 == 1);
        std::string cid = (i < 10 ? "conv-0" : "conv-") + std::to_string(i);
        fs::path cdir = fs::path(isCli ? cliBrain : ideBrain) / cid;
        fs::path logDir = cdir / ".system_generated" / "logs";
        fs::create_directories(logDir);
        fs::path transcriptPath = logDir / "transcript.jsonl";

        if (i == 39) {
            //Newest conversation has task.md heading
            std::ofstream tf(cdir / "task.md");
            tf << "# Repair Antimatter Gateway Task\n\nSome task details\n";
            tf.close();
        }

        std::ofstream lf(transcriptPath);
        lf << "{\"step_index\":0,\"type\":\"USER_INPUT\",\"content\":\"<USER_REQUEST>\\nPrompt for conversation " << i << "\\n</USER_REQUEST>\"}\n";
        lf.close();

        // Set explicit mtime: conv-00 oldest, conv-39 newest
        auto targetTp = std::chrono::system_clock::from_time_t(baseTime + i * 60);
        auto fileTp = fs::file_time_type::clock::now() +
            std::chrono::duration_cast<fs::file_time_type::duration>(targetTp - std::chrono::system_clock::now());
        fs::last_write_time(transcriptPath, fileTp);

        if (i == 38) {
            std::ofstream pf(fs::path(presenceDir) / (cid + ".lock"));
            pf << "lock";
            pf.close();
        }
    }

    StateStore &stateStore = StateStore::getInstance();
    HistoryStore historyStore;
    WebConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.endpointsEnabled = false;

    WebServer server(stateStore, historyStore, config);
    assert(server.start(true));
    int port = server.getPort();

    httplib::Client client("http://127.0.0.1:" + std::to_string(port));
    httplib::Headers headers = {{"X-Mobile-Token", validToken}};

    auto res = client.Get("/api/agent/conversations", headers);
    assert(res && res->status == 200);
    json body = json::parse(res->body);
    assert(body.contains("conversations") && body["conversations"].is_array());
    auto convs = body["conversations"];

    // All 40 conversations should be returned (default limit 50 >= 40)
    assert(convs.size() == 40);

    // Verify strictly descending timestamp order (conv-39 first, conv-00 last)
    bool sawIde = false;
    bool sawCli = false;
    for (size_t idx = 0; idx < convs.size(); ++idx) {
        int expectedNum = 39 - static_cast<int>(idx);
        std::string expectedId = (expectedNum < 10 ? "conv-0" : "conv-") + std::to_string(expectedNum);
        assert(convs[idx].value("id", "") == expectedId);
        std::string src = convs[idx].value("source", "");
        if (src == "ide") sawIde = true;
        if (src == "cli") sawCli = true;
        if (idx > 0) {
            uint64_t prevTs = convs[idx - 1].value("timestamp", uint64_t(0));
            uint64_t curTs = convs[idx].value("timestamp", uint64_t(0));
            assert(prevTs >= curTs);
        }
    }
    assert(sawIde && sawCli);

    // conv-39 title should come from task.md "# Repair Antimatter Gateway Task"
    assert(convs[0].value("title", "") == "Repair Antimatter Gateway Task");
    // conv-38 should have active == true from presence lock, and title from USER_INPUT
    assert(convs[1].value("title", "") == "Prompt for conversation 38");
    assert(convs[1].value("active", false) == true);

    server.stop();
    unsetenv("AIMON_IDE_BRAIN_DIR");
    unsetenv("AIMON_CLI_BRAIN_DIR");
    unsetenv("AIMON_CLI_PRESENCE_DIR");
    fs::remove_all(baseDir);
    std::cout << "[TestMobileGateway] testMultiRootBrainConversationDiscovery passed!" << std::endl;
}

static void testTranscriptJsonlParsingAndRapidCheck() {
    std::cout << "[TestMobileGateway] Running testTranscriptJsonlParsingAndRapidCheck..." << std::endl;
    std::string baseDir = "/tmp/aimon_env2_transcript_" + std::to_string(getpid());
    std::string ideBrain = baseDir + "/ide_brain";
    std::string cliBrain = baseDir + "/cli_brain";
    std::string tempMobileDb = baseDir + "/mobile_devices.json";
    fs::remove_all(baseDir);
    fs::create_directories(ideBrain);
    fs::create_directories(cliBrain);

    setenv("AIMON_IDE_BRAIN_DIR", ideBrain.c_str(), 1);
    setenv("AIMON_CLI_BRAIN_DIR", cliBrain.c_str(), 1);

    assert(MobileGateway::getInstance().init(tempMobileDb));
    std::string secret = MobileGateway::getInstance().createPairingSecret(120);
    std::string validToken;
    assert(MobileGateway::getInstance().verifyPairingCode(secret, "env2-trans-dev", "Test Device", validToken));

    // Write a 160-step transcript in CLI brain to verify >100 lines are NOT truncated
    std::string cid = "cli-long-conv-160";
    fs::path logDir = fs::path(cliBrain) / cid / ".system_generated" / "logs";
    fs::create_directories(logDir);
    fs::path transcriptPath = logDir / "transcript.jsonl";
    {
        std::ofstream lf(transcriptPath);
        for (int i = 0; i < 160; ++i) {
            if (i % 2 == 0) {
                json step = {
                    {"step_index", i},
                    {"type", "USER_INPUT"},
                    {"content", "<USER_REQUEST>\nUser question " + std::to_string(i) + "\n</USER_REQUEST>"}
                };
                lf << step.dump() << "\n";
            } else {
                json step = {
                    {"step_index", i},
                    {"type", "PLANNER_RESPONSE"},
                    {"thinking", "Internal reasoning for step " + std::to_string(i)},
                    {"content", "Final answer for step " + std::to_string(i)},
                    {"tool_calls", json::array({
                        {
                            {"name", "run_command"},
                            {"args", {
                                {"CommandLine", "echo step " + std::to_string(i)},
                                {"toolAction", "Running echo"},
                                {"toolSummary", "Echo step"}
                            }}
                        }
                    })}
                };
                lf << step.dump() << "\n";
            }
        }
    }

    StateStore &stateStore = StateStore::getInstance();
    HistoryStore historyStore;
    WebConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.endpointsEnabled = false;

    WebServer server(stateStore, historyStore, config);
    assert(server.start(true));
    int port = server.getPort();

    httplib::Client client("http://127.0.0.1:" + std::to_string(port));
    httplib::Headers headers = {{"X-Mobile-Token", validToken}};

    // 1. Fetch full transcript (all 160 JSONL lines, no 100-line cap)
    auto resFull = client.Get(("/api/agent/transcript?id=" + cid).c_str(), headers);
    assert(resFull && resFull->status == 200);
    json fullBody = json::parse(resFull->body);
    assert(fullBody.value("conversationId", "") == cid);
    assert(fullBody.contains("steps") && fullBody["steps"].is_array());
    auto steps = fullBody["steps"];
    assert(steps.size() >= 160);

    // Check separation of thinking, text, and tool_calls on step_index 1
    bool foundPlannerStep1 = false;
    for (const auto &s : steps) {
        if (s.value("stepIndex", -1) == 1) {
            foundPlannerStep1 = true;
            assert(s.value("type", "") == "PLANNER_RESPONSE");
            assert(s.value("thinking", "") == "Internal reasoning for step 1");
            assert(s.value("plannerResponse", "") == "Internal reasoning for step 1");
            assert(s.value("text", "") == "Final answer for step 1");
            assert(s.contains("tool_calls") && s["tool_calls"].is_array());
            assert(s["tool_calls"].size() == 1);
            assert(s["tool_calls"][0].value("name", "") == "run_command");
            assert(s["tool_calls"][0].value("toolAction", "") == "Running echo");
            assert(s["tool_calls"][0].value("toolSummary", "") == "Echo step");
            break;
        }
    }
    assert(foundPlannerStep1);

    // 2. Fetch incremental steps with ?since=150
    auto resSince = client.Get(("/api/agent/transcript?id=" + cid + "&since=150").c_str(), headers);
    assert(resSince && resSince->status == 200);
    json sinceBody = json::parse(resSince->body);
    auto sinceSteps = sinceBody["steps"];
    assert(!sinceSteps.empty());
    for (const auto &s : sinceSteps) {
        assert(s.value("stepIndex", -1) > 150);
    }

    // 3. RapidCheck property test across arbitrary user input, thinking, content, and since offsets
    bool rcOk = rc::check("transcript JSONL parser preserves arbitrary thinking/content and honors since filter",
                          [&]() {
        const auto userText = *rc::gen::container<std::string>(rc::gen::inRange<char>(32, 126));
        const auto thinkText = *rc::gen::container<std::string>(rc::gen::inRange<char>(32, 126));
        const auto replyText = *rc::gen::container<std::string>(rc::gen::inRange<char>(32, 126));
        const int totalPairs = *rc::gen::inRange(2, 12);
        const int sinceIdx = *rc::gen::inRange(0, totalPairs * 2 - 1);

        std::string rcCid = "rc-conv";
        fs::path rcLogDir = fs::path(ideBrain) / rcCid / ".system_generated" / "logs";
        fs::create_directories(rcLogDir);
        fs::path rcPath = rcLogDir / "transcript.jsonl";
        {
            std::ofstream out(rcPath, std::ios::trunc);
            for (int p = 0; p < totalPairs; ++p) {
                int uIdx = p * 2;
                int aIdx = p * 2 + 1;
                json uStep = {
                    {"step_index", uIdx},
                    {"type", "USER_INPUT"},
                    {"content", userText}
                };
                json aStep = {
                    {"step_index", aIdx},
                    {"type", "PLANNER_RESPONSE"},
                    {"thinking", thinkText},
                    {"content", replyText},
                    {"tool_calls", json::array()}
                };
                out << uStep.dump() << "\n" << aStep.dump() << "\n";
            }
        }

        auto rcRes = client.Get(("/api/agent/transcript?id=" + rcCid + "&since=" + std::to_string(sinceIdx)).c_str(), headers);
        RC_ASSERT(rcRes != nullptr);
        RC_ASSERT(rcRes->status == 200);
        json parsed = json::parse(rcRes->body);
        RC_ASSERT(parsed.contains("steps"));
        for (const auto &st : parsed["steps"]) {
            int sIdx = st.value("stepIndex", -1);
            RC_ASSERT(sIdx > sinceIdx);
            if (st.value("type", "") == "PLANNER_RESPONSE") {
                RC_ASSERT(st.value("thinking", "") == thinkText);
                RC_ASSERT(st.value("text", "") == replyText);
            }
        }
    });
    assert(rcOk);

    server.stop();
    unsetenv("AIMON_IDE_BRAIN_DIR");
    unsetenv("AIMON_CLI_BRAIN_DIR");
    fs::remove_all(baseDir);
    std::cout << "[TestMobileGateway] testTranscriptJsonlParsingAndRapidCheck passed!" << std::endl;
}

static void testWorkspaceRecursiveTree() {
    std::cout << "[TestMobileGateway] Running testWorkspaceRecursiveTree..." << std::endl;
    std::string baseDir = "/tmp/aimon_env2_ws_" + std::to_string(getpid());
    std::string wsRoot = baseDir + "/workspace";
    std::string tempMobileDb = baseDir + "/mobile_devices.json";
    fs::remove_all(baseDir);

    // Create nested structure 4 levels deep plus excluded directories
    fs::create_directories(wsRoot + "/a/b/c/d");
    fs::create_directories(wsRoot + "/.git/objects");
    fs::create_directories(wsRoot + "/build/objs");
    fs::create_directories(wsRoot + "/node_modules/pkg");
    fs::create_directories(wsRoot + "/third_party/lib");

    {
        std::ofstream f1(wsRoot + "/a/b/c/d/deep_file.cxx");
        f1 << "int main() { return 0; }\n";
        std::ofstream f2(wsRoot + "/.git/objects/secret");
        f2 << "git";
        std::ofstream f3(wsRoot + "/build/objs/out.o");
        f3 << "obj";
    }

    setenv("AIMON_WORKSPACE_ROOT", wsRoot.c_str(), 1);
    assert(MobileGateway::getInstance().init(tempMobileDb));
    std::string secret = MobileGateway::getInstance().createPairingSecret(120);
    std::string validToken;
    assert(MobileGateway::getInstance().verifyPairingCode(secret, "env2-ws-dev", "Test Device", validToken));

    StateStore &stateStore = StateStore::getInstance();
    HistoryStore historyStore;
    WebConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.endpointsEnabled = false;

    WebServer server(stateStore, historyStore, config);
    assert(server.start(true));
    int port = server.getPort();

    httplib::Client client("http://127.0.0.1:" + std::to_string(port));
    httplib::Headers headers = {{"X-Mobile-Token", validToken}};

    // 1. Recursive tree from root finds a/b/c/d/deep_file.cxx and skips .git, build, node_modules, third_party
    auto resTree = client.Get("/api/workspace/tree", headers);
    assert(resTree && resTree->status == 200);
    std::string rawTree = resTree->body;
    assert(rawTree.find("a/b/c/d/deep_file.cxx") != std::string::npos);
    assert(rawTree.find(".git") == std::string::npos);
    assert(rawTree.find("node_modules") == std::string::npos);
    assert(rawTree.find("third_party") == std::string::npos);
    assert(rawTree.find("out.o") == std::string::npos);

    // 2. Subpath query ?path=a/b/c returns d and d/deep_file.cxx
    auto resSub = client.Get("/api/workspace/tree?path=a/b/c", headers);
    assert(resSub && resSub->status == 200);
    assert(resSub->body.find("a/b/c/d/deep_file.cxx") != std::string::npos);

    // 3. Path traversal ?path=../../etc is rejected with 400
    auto resTraversal = client.Get("/api/workspace/tree?path=../../etc", headers);
    assert(resTraversal && resTraversal->status == 400);

    server.stop();
    unsetenv("AIMON_WORKSPACE_ROOT");
    fs::remove_all(baseDir);
    std::cout << "[TestMobileGateway] testWorkspaceRecursiveTree passed!" << std::endl;
}

static void testAgentPromptNoFakeElizaAndRealBridge() {
    std::cout << "[TestMobileGateway] Running testAgentPromptNoFakeElizaAndRealBridge..." << std::endl;
    std::string baseDir = "/tmp/aimon_env2_prompt_" + std::to_string(getpid());
    std::string tempMobileDb = baseDir + "/mobile_devices.json";
    fs::remove_all(baseDir);
    fs::create_directories(baseDir);

    assert(MobileGateway::getInstance().init(tempMobileDb));
    std::string secret = MobileGateway::getInstance().createPairingSecret(120);
    std::string validToken;
    assert(MobileGateway::getInstance().verifyPairingCode(secret, "env2-prompt-dev", "Test Device", validToken));

    StateStore &stateStore = StateStore::getInstance();
    HistoryStore historyStore;
    WebConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.endpointsEnabled = false;

    WebServer server(stateStore, historyStore, config);
    assert(server.start(true));
    int port = server.getPort();

    httplib::Client client("http://127.0.0.1:" + std::to_string(port));
    httplib::Headers headers = {{"X-Mobile-Token", validToken}};

    // 1. Point AIMON_AGY_BIN to a nonexistent binary to simulate no reachable agent
    setenv("AIMON_AGY_BIN", "/tmp/nonexistent_agy_binary_for_test", 1);
    json helloPrompt = {
        {"text", "hello what time is it status"},
        {"conversationId", "fake-conv-id"}
    };
    auto resOffline = client.Post("/api/agent/prompt", headers, helloPrompt.dump(), "application/json");
    assert(resOffline != nullptr);
    // Must NOT return 200 with Eliza canned responses! Must return 503 Service Unavailable
    assert(resOffline->status == 503);
    assert(resOffline->body.find("Hello! I am Google Antigravity") == std::string::npos);
    assert(resOffline->body.find("Total credits: 200,000") == std::string::npos);
    assert(resOffline->body.find("Acknowledged:") == std::string::npos);

    // 2. Point AIMON_AGY_BIN to a mock agy script that verifies arguments and environment variables
    std::string mockAgy = baseDir + "/mock_agy.sh";
    {
        std::ofstream mf(mockAgy);
        mf << "#!/bin/sh\n"
           << "if [ \"$1\" = \"agentapi\" ] && [ \"$2\" = \"send-message\" ]; then\n"
           << "  echo \"{\\\"status\\\":\\\"sent\\\",\\\"recipient\\\":\\\"$3\\\",\\\"prompt\\\":\\\"$4\\\",\\\"ls_port\\\":\\\"$AGY_LS_PORT\\\"}\"\n"
           << "  exit 0\n"
           << "fi\n"
           << "if [ \"$1\" = \"agentapi\" ] && [ \"$2\" = \"new-conversation\" ]; then\n"
           << "  echo \"{\\\"response\\\":{\\\"newConversation\\\":{\\\"conversationId\\\":\\\"new-conv-999\\\",\\\"prompt\\\":\\\"$3\\\"}}}\"\n"
           << "  exit 0\n"
           << "fi\n"
           << "exit 1\n";
    }
    chmod(mockAgy.c_str(), 0755);
    setenv("AIMON_AGY_BIN", mockAgy.c_str(), 1);
    setenv("AGY_LS_PORT", "46799", 1);
    setenv("AGY_LS_CSRF_TOKEN", "test-csrf-token", 1);

    json sendPrompt = {
        {"text", "Run unit tests now"},
        {"conversationId", "conv-existing-123"}
    };
    auto resSend = client.Post("/api/agent/prompt", headers, sendPrompt.dump(), "application/json");
    assert(resSend && resSend->status == 200);
    json sendResp = json::parse(resSend->body);
    assert(sendResp.value("status", "") == "dispatched");
    assert(sendResp.value("conversationId", "") == "conv-existing-123");
    assert(sendResp.value("output", "").find("Run unit tests now") != std::string::npos);

    json newPrompt = {
        {"text", "Start a new task"},
        {"conversationId", "new"}
    };
    auto resNew = client.Post("/api/agent/prompt", headers, newPrompt.dump(), "application/json");
    assert(resNew && resNew->status == 200);
    json newResp = json::parse(resNew->body);
    assert(newResp.value("status", "") == "dispatched");
    assert(newResp.value("conversationId", "") == "new-conv-999");

    // 3. Verify direct ConnectRPC StartCascade + SendUserCascadeMessage + GetConversationMetadata routing
    httplib::Server mockLs;
    std::string lastSentCascadeId;
    std::string lastSentText;
    mockLs.Post("/exa.language_server_pb.LanguageServerService/GetConversationMetadata",
                [](const httplib::Request &req, httplib::Response &res) {
        if (req.get_header_value("x-codeium-csrf-token") != "rpc-csrf-123") {
            res.status = 401;
            return;
        }
        auto j = json::parse(req.body, nullptr, false);
        if (j.value("conversationId", "") == "rpc-owned-conv-42") {
            res.set_content("{\"metadata\":{\"workspaces\":[]}}", "application/json");
        } else {
            res.status = 404;
            res.set_content("{\"code\":\"not_found\",\"message\":\"trajectory not found\"}", "application/json");
        }
    });
    mockLs.Post("/exa.language_server_pb.LanguageServerService/GetAllCascadeTrajectories",
                [](const httplib::Request &req, httplib::Response &res) {
        if (req.get_header_value("x-codeium-csrf-token") != "rpc-csrf-123") {
            res.status = 401;
            return;
        }
        res.set_content("{\"trajectorySummaries\":{\"rpc-owned-conv-42\":{}}}", "application/json");
    });
    mockLs.Post("/exa.language_server_pb.LanguageServerService/StartCascade",
                [](const httplib::Request &req, httplib::Response &res) {
        if (req.get_header_value("x-codeium-csrf-token") != "rpc-csrf-123") {
            res.status = 401;
            return;
        }
        res.set_content("{\"cascadeId\":\"rpc-started-conv-777\"}", "application/json");
    });
    mockLs.Post("/exa.language_server_pb.LanguageServerService/SendUserCascadeMessage",
                [&](const httplib::Request &req, httplib::Response &res) {
        if (req.get_header_value("x-codeium-csrf-token") != "rpc-csrf-123") {
            res.status = 401;
            return;
        }
        auto j = json::parse(req.body, nullptr, false);
        lastSentCascadeId = j.value("cascadeId", "");
        if (j.contains("items") && j["items"].is_array() && !j["items"].empty()) {
            lastSentText = j["items"][0].value("text", "");
        }
        res.set_content("{}", "application/json");
    });
    int lsPort = mockLs.bind_to_any_port("127.0.0.1");
    std::thread lsThread([&]() { mockLs.listen_after_bind(); });

    setenv("AGY_LS_PORT", std::to_string(lsPort).c_str(), 1);
    setenv("AGY_LS_CSRF_TOKEN", "rpc-csrf-123", 1);

    json rpcExistingPrompt = {
        {"text", "Hello via ConnectRPC"},
        {"conversationId", "rpc-owned-conv-42"}
    };
    auto resRpcExisting = client.Post("/api/agent/prompt", headers, rpcExistingPrompt.dump(), "application/json");
    assert(resRpcExisting && resRpcExisting->status == 200);
    json rpcExistingBody = json::parse(resRpcExisting->body);
    assert(rpcExistingBody.value("conversationId", "") == "rpc-owned-conv-42");
    assert(lastSentCascadeId == "rpc-owned-conv-42");
    assert(lastSentText == "Hello via ConnectRPC");

    json rpcNewPrompt = {
        {"text", "New conversation via ConnectRPC"},
        {"conversationId", "new"}
    };
    auto resRpcNew = client.Post("/api/agent/prompt", headers, rpcNewPrompt.dump(), "application/json");
    assert(resRpcNew && resRpcNew->status == 200);
    json rpcNewBody = json::parse(resRpcNew->body);
    assert(rpcNewBody.value("conversationId", "") == "rpc-started-conv-777");
    assert(lastSentCascadeId == "rpc-started-conv-777");
    assert(lastSentText == "New conversation via ConnectRPC");

    mockLs.stop();
    if (lsThread.joinable()) lsThread.join();

    server.stop();
    unsetenv("AIMON_AGY_BIN");
    unsetenv("AGY_LS_PORT");
    unsetenv("AGY_LS_CSRF_TOKEN");
    fs::remove_all(baseDir);
    std::cout << "[TestMobileGateway] testAgentPromptNoFakeElizaAndRealBridge passed!" << std::endl;
}

static void testAgentPromptPreservesCascadeConfigAndModelM418() {
    std::cout << "[TestMobileGateway] Running testAgentPromptPreservesCascadeConfigAndModelM418..." << std::endl;
    std::string baseDir = "/tmp/aimon_m418_test_" + std::to_string(getpid());
    std::string ideBrain = baseDir + "/ide_brain";
    std::string cliBrain = baseDir + "/cli_brain";
    std::string wsRoot = baseDir + "/workspace";
    std::string tempMobileDb = baseDir + "/mobile_devices.json";
    fs::remove_all(baseDir);
    fs::create_directories(ideBrain);
    fs::create_directories(cliBrain);
    fs::create_directories(wsRoot);

    setenv("AIMON_IDE_BRAIN_DIR", ideBrain.c_str(), 1);
    setenv("AIMON_CLI_BRAIN_DIR", cliBrain.c_str(), 1);
    setenv("AIMON_WORKSPACE_ROOT", wsRoot.c_str(), 1);

    // Create a mock agy binary so the binary existence check passes and CLI fallback can be verified
    std::string mockAgy = baseDir + "/mock_agy.sh";
    {
        std::ofstream mf(mockAgy);
        mf << "#!/bin/sh\n"
           << "if [ \"$1\" = \"agentapi\" ] && [ \"$2\" = \"new-conversation\" ]; then\n"
           << "  echo \"{\\\"response\\\":{\\\"newConversation\\\":{\\\"conversationId\\\":\\\"cli-new-conv\\\",\\\"modelFlag\\\":\\\"$3\\\",\\\"prompt\\\":\\\"$4\\\"}}}\"\n"
           << "  exit 0\n"
           << "fi\n"
           << "exit 1\n";
    }
    chmod(mockAgy.c_str(), 0755);
    setenv("AIMON_AGY_BIN", mockAgy.c_str(), 1);

    assert(MobileGateway::getInstance().init(tempMobileDb));
    std::string secret = MobileGateway::getInstance().createPairingSecret(120);
    std::string validToken;
    assert(MobileGateway::getInstance().verifyPairingCode(secret, "m418-dev", "Test Device", validToken));

    // Mock Antigravity Language Server with a trajectory whose earlier executorMetadatas[0] has M418 + customizationConfig
    // and whose latest executorMetadatas[1] was corrupted by a prior M322 prompt without customizationConfig.
    httplib::Server mockLs;
    std::mutex lsMutex;
    json currentExecutorMetadatas = json::array({
        {
            {"cascadeConfig", {
                {"plannerConfig", {
                    {"planModel", "MODEL_PLACEHOLDER_M418"},
                    {"modelName", "gemini-4-argon-medium"},
                    {"useAiCredits", false},
                    {"customizationConfig", {
                        {"enableJsonHooks", true},
                        {"workspacePaths", json::array({wsRoot})}
                    }}
                }},
                {"dynamicWorkspaceUris", json::array({"file://" + wsRoot})}
            }}
        },
        {
            {"cascadeConfig", {
                {"plannerConfig", {
                    {"planModel", "MODEL_PLACEHOLDER_M322"},
                    {"modelName", "gemini-3.8-flash-tiered"}
                }}
            }}
        }
    });
    json lastSentPayload;

    mockLs.Post("/exa.language_server_pb.LanguageServerService/GetConversationMetadata",
                [](const httplib::Request &req, httplib::Response &res) {
        if (req.get_header_value("x-codeium-csrf-token") != "m418-csrf") {
            res.status = 401;
            return;
        }
        auto j = json::parse(req.body, nullptr, false);
        if (j.value("conversationId", "") == "conv-corrupted-m322") {
            res.set_content("{\"metadata\":{\"workspaces\":[]}}", "application/json");
        } else {
            res.status = 404;
            res.set_content("{\"code\":\"not_found\"}", "application/json");
        }
    });

    mockLs.Post("/exa.language_server_pb.LanguageServerService/GetAllCascadeTrajectories",
                [](const httplib::Request &req, httplib::Response &res) {
        if (req.get_header_value("x-codeium-csrf-token") != "m418-csrf") {
            res.status = 401;
            return;
        }
        res.set_content("{\"trajectorySummaries\":{\"conv-corrupted-m322\":{}}}", "application/json");
    });

    mockLs.Post("/exa.language_server_pb.LanguageServerService/GetCascadeTrajectory",
                [&](const httplib::Request &req, httplib::Response &res) {
        if (req.get_header_value("x-codeium-csrf-token") != "m418-csrf") {
            res.status = 401;
            return;
        }
        auto j = json::parse(req.body, nullptr, false);
        std::string reqCid = j.value("cascadeId", "");
        if (reqCid == "conv-corrupted-m322") {
            std::lock_guard<std::mutex> lk(lsMutex);
            json tj = {
                {"trajectory", {
                    {"steps", json::array({
                        {
                            {"type", "CORTEX_STEP_TYPE_USER_INPUT"},
                            {"userInput", {{"userResponse", "Check model preservation"}}}
                        },
                        {
                            {"type", "CORTEX_STEP_TYPE_PLANNER_RESPONSE"},
                            {"plannerResponse", {{"response", "Running on Gemini 4 Argon Medium"}, {"thinking", "Checking..."}}}
                        }
                    })},
                    {"executorMetadatas", currentExecutorMetadatas},
                    {"generatorMetadata", json::array({
                        {{"chatModel", {{"model", "MODEL_PLACEHOLDER_M418"}}}}
                    })}
                }}
            };
            res.set_content(tj.dump(), "application/json");
            return;
        }
        res.status = 404;
        res.set_content("{\"code\":\"not_found\"}", "application/json");
    });

    mockLs.Post("/exa.language_server_pb.LanguageServerService/StartCascade",
                [](const httplib::Request &req, httplib::Response &res) {
        if (req.get_header_value("x-codeium-csrf-token") != "m418-csrf") {
            res.status = 401;
            return;
        }
        res.set_content("{\"cascadeId\":\"fresh-cascade-m418\"}", "application/json");
    });

    mockLs.Post("/exa.language_server_pb.LanguageServerService/SendUserCascadeMessage",
                [&](const httplib::Request &req, httplib::Response &res) {
        if (req.get_header_value("x-codeium-csrf-token") != "m418-csrf") {
            res.status = 401;
            return;
        }
        std::lock_guard<std::mutex> lk(lsMutex);
        lastSentPayload = json::parse(req.body, nullptr, false);
        res.set_content("{}", "application/json");
    });

    int lsPort = mockLs.bind_to_any_port("127.0.0.1");
    std::thread lsThread([&]() { mockLs.listen_after_bind(); });

    setenv("AGY_LS_PORT", std::to_string(lsPort).c_str(), 1);
    setenv("AGY_LS_CSRF_TOKEN", "m418-csrf", 1);

    StateStore &stateStore = StateStore::getInstance();
    HistoryStore historyStore;
    WebConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.endpointsEnabled = false;

    WebServer server(stateStore, historyStore, config);
    assert(server.start(true));
    int port = server.getPort();

    httplib::Client client("http://127.0.0.1:" + std::to_string(port));
    httplib::Headers headers = {{"X-Mobile-Token", validToken}};

    // 1. Dispatch POST /api/agent/prompt to conv-corrupted-m322 and verify SendUserCascadeMessage
    // restores MODEL_PLACEHOLDER_M418, customizationConfig, and dynamicWorkspaceUris!
    json promptReq = {
        {"text", "Continue envelope execution"},
        {"conversationId", "conv-corrupted-m322"}
    };
    auto resPrompt = client.Post("/api/agent/prompt", headers, promptReq.dump(), "application/json");
    assert(resPrompt && resPrompt->status == 200);
    {
        std::lock_guard<std::mutex> lk(lsMutex);
        assert(lastSentPayload.is_object());
        assert(lastSentPayload.contains("cascadeConfig") && lastSentPayload["cascadeConfig"].is_object());
        const auto &cc = lastSentPayload["cascadeConfig"];
        assert(cc.contains("plannerConfig") && cc["plannerConfig"].is_object());
        const auto &pc = cc["plannerConfig"];
        assert(pc.value("planModel", "") == "MODEL_PLACEHOLDER_M418");
        assert(pc.contains("customizationConfig") && pc["customizationConfig"].is_object());
        assert(pc["customizationConfig"].value("enableJsonHooks", false) == true);
        assert(cc.contains("dynamicWorkspaceUris") && cc["dynamicWorkspaceUris"].is_array());
        assert(!cc["dynamicWorkspaceUris"].empty());
        assert(cc["dynamicWorkspaceUris"][0].get<std::string>() == "file://" + wsRoot);
    }

    // 2. Verify GET /api/agent/transcript (live RPC trajectory branch) returns model = "gemini-4-argon-medium" (never "gemini-2.5-pro")
    auto resLiveTrans = client.Get("/api/agent/transcript?id=conv-corrupted-m322", headers);
    assert(resLiveTrans && resLiveTrans->status == 200);
    json liveTransJson = json::parse(resLiveTrans->body);
    assert(liveTransJson.value("model", "") == "gemini-4-argon-medium");

    // 3. Verify GET /api/agent/transcript (JSONL file branch) also returns "gemini-4-argon-medium" (never "gemini-2.5-pro")
    fs::path jsonlDir = fs::path(cliBrain) / "jsonl-only-conv" / ".system_generated" / "logs";
    fs::create_directories(jsonlDir);
    {
        std::ofstream jf(jsonlDir / "transcript.jsonl");
        jf << "{\"step_index\":0,\"type\":\"USER_INPUT\",\"content\":\"Hello\"}\n";
    }
    auto resJsonlTrans = client.Get("/api/agent/transcript?id=jsonl-only-conv", headers);
    assert(resJsonlTrans && resJsonlTrans->status == 200);
    json jsonlTransJson = json::parse(resJsonlTrans->body);
    assert(jsonlTransJson.value("model", "") == "gemini-4-argon-medium");

    // 3b. Verify new cascade ("conversationId": "new") clones workspace cascadeConfig via GetAllCascadeTrajectories
    json newCascadeReq = {
        {"text", "Start new conversation with preserved workspace config"},
        {"conversationId", "new"}
    };
    auto resNewCascade = client.Post("/api/agent/prompt", headers, newCascadeReq.dump(), "application/json");
    assert(resNewCascade && resNewCascade->status == 200);
    {
        std::lock_guard<std::mutex> lk(lsMutex);
        assert(lastSentPayload.value("cascadeId", "") == "fresh-cascade-m418");
        const auto &cc = lastSentPayload["cascadeConfig"];
        assert(cc["plannerConfig"].value("planModel", "") == "MODEL_PLACEHOLDER_M418");
        assert(cc["plannerConfig"]["customizationConfig"].value("enableJsonHooks", false) == true);
        assert(cc["dynamicWorkspaceUris"][0].get<std::string>() == "file://" + wsRoot);
    }

    // 3c. Verify explicit model overrides ("flash", "pro", "MODEL_CUSTOM_999") preserve customizationConfig
    json flashReq = {
        {"text", "Quick flash query"},
        {"conversationId", "conv-corrupted-m322"},
        {"model", "flash"}
    };
    auto resFlash = client.Post("/api/agent/prompt", headers, flashReq.dump(), "application/json");
    assert(resFlash && resFlash->status == 200);
    {
        std::lock_guard<std::mutex> lk(lsMutex);
        const auto &pc = lastSentPayload["cascadeConfig"]["plannerConfig"];
        assert(pc.value("planModel", "") == "MODEL_PLACEHOLDER_M322");
        assert(pc.value("modelName", "") == "gemini-3.8-flash-tiered");
        assert(pc["customizationConfig"].value("enableJsonHooks", false) == true);
    }

    json proReq = {
        {"text", "Explicit pro query"},
        {"conversationId", "conv-corrupted-m322"},
        {"model", "pro"}
    };
    auto resPro = client.Post("/api/agent/prompt", headers, proReq.dump(), "application/json");
    assert(resPro && resPro->status == 200);
    {
        std::lock_guard<std::mutex> lk(lsMutex);
        const auto &pc = lastSentPayload["cascadeConfig"]["plannerConfig"];
        assert(pc.value("planModel", "") == "MODEL_PLACEHOLDER_M418");
        assert(pc.value("modelName", "") == "gemini-4-argon-medium");
    }

    json customModelReq = {
        {"text", "Custom enum query"},
        {"conversationId", "conv-corrupted-m322"},
        {"model", "MODEL_CUSTOM_999"}
    };
    auto resCustomModel = client.Post("/api/agent/prompt", headers, customModelReq.dump(), "application/json");
    assert(resCustomModel && resCustomModel->status == 200);
    {
        std::lock_guard<std::mutex> lk(lsMutex);
        const auto &pc = lastSentPayload["cascadeConfig"]["plannerConfig"];
        assert(pc.value("planModel", "") == "MODEL_CUSTOM_999");
    }

    // 4. RapidCheck property: across arbitrary sequences of corrupted M322 turns appended after a valid M418 turn,
    // POST /api/agent/prompt always recovers MODEL_PLACEHOLDER_M418, customizationConfig, and dynamicWorkspaceUris.
    bool rcOk = rc::check("POST /api/agent/prompt recovers M418 and customizationConfig across arbitrary trailing M322 turns",
                          [&]() {
        const int trailingM322Count = *rc::gen::inRange(0, 6);
        const auto wsSubdir = *rc::gen::element<std::string>("aimon", "netmon", "meshmon", "robohero");
        std::string expectedUri = "file:///workspace/" + wsSubdir;

        {
            std::lock_guard<std::mutex> lk(lsMutex);
            currentExecutorMetadatas = json::array();
            currentExecutorMetadatas.push_back({
                {"cascadeConfig", {
                    {"plannerConfig", {
                        {"planModel", "MODEL_PLACEHOLDER_M418"},
                        {"modelName", "gemini-4-argon-medium"},
                        {"customizationConfig", {
                            {"enableJsonHooks", true}
                        }}
                    }},
                    {"dynamicWorkspaceUris", json::array({expectedUri})}
                }}
            });
            for (int k = 0; k < trailingM322Count; ++k) {
                currentExecutorMetadatas.push_back({
                    {"cascadeConfig", {
                        {"plannerConfig", {
                            {"planModel", "MODEL_PLACEHOLDER_M322"},
                            {"modelName", "gemini-3.8-flash-tiered"}
                        }}
                    }}
                });
            }
            lastSentPayload = json::object();
        }

        auto r = client.Post("/api/agent/prompt", headers, promptReq.dump(), "application/json");
        RC_ASSERT(r != nullptr);
        RC_ASSERT(r->status == 200);

        std::lock_guard<std::mutex> lk(lsMutex);
        RC_ASSERT(lastSentPayload.contains("cascadeConfig"));
        const auto &cc = lastSentPayload["cascadeConfig"];
        RC_ASSERT(cc.contains("plannerConfig"));
        RC_ASSERT(cc["plannerConfig"].value("planModel", "") == "MODEL_PLACEHOLDER_M418");
        RC_ASSERT(cc["plannerConfig"].contains("customizationConfig"));
        RC_ASSERT(cc["plannerConfig"]["customizationConfig"].value("enableJsonHooks", false) == true);
        RC_ASSERT(cc.contains("dynamicWorkspaceUris"));
        RC_ASSERT(cc["dynamicWorkspaceUris"].is_array() && !cc["dynamicWorkspaceUris"].empty());
        RC_ASSERT(cc["dynamicWorkspaceUris"][0].get<std::string>() == expectedUri);
    });
    assert(rcOk);

    mockLs.stop();
    if (lsThread.joinable()) lsThread.join();

    // 5. Verify agy agentapi CLI fallback passes --model=pro by default, and --model=flash / --model=flash_lite when requested
    setenv("AGY_LS_PORT", "46798", 1);
    json cliDefaultReq = {{"text", "CLI default new"}, {"conversationId", "new"}};
    auto resCliDefault = client.Post("/api/agent/prompt", headers, cliDefaultReq.dump(), "application/json");
    assert(resCliDefault && resCliDefault->status == 200);
    assert(resCliDefault->body.find("--model=pro") != std::string::npos);

    json cliFlashReq = {{"text", "CLI flash new"}, {"conversationId", "new"}, {"model", "flash"}};
    auto resCliFlash = client.Post("/api/agent/prompt", headers, cliFlashReq.dump(), "application/json");
    assert(resCliFlash && resCliFlash->status == 200);
    assert(resCliFlash->body.find("--model=flash") != std::string::npos);

    json cliFlashLiteReq = {{"text", "CLI flash_lite new"}, {"conversationId", "new"}, {"model", "flash_lite"}};
    auto resCliFlashLite = client.Post("/api/agent/prompt", headers, cliFlashLiteReq.dump(), "application/json");
    assert(resCliFlashLite && resCliFlashLite->status == 200);
    assert(resCliFlashLite->body.find("--model=flash_lite") != std::string::npos);

    json cliArgonHighReq = {{"text", "CLI Argon High new"}, {"conversationId", "new"}, {"model", "Gemini 4 Argon (High)"}};
    auto resCliArgonHigh = client.Post("/api/agent/prompt", headers, cliArgonHighReq.dump(), "application/json");
    assert(resCliArgonHigh && resCliArgonHigh->status == 200);
    assert(resCliArgonHigh->body.find("--model=gemini-4-argon-high") != std::string::npos);

    json cliArgonMedReq = {{"text", "CLI Argon Med new"}, {"conversationId", "new"}, {"model", "Gemini 4 Argon (Medium)"}};
    auto resCliArgonMed = client.Post("/api/agent/prompt", headers, cliArgonMedReq.dump(), "application/json");
    assert(resCliArgonMed && resCliArgonMed->status == 200);
    assert(resCliArgonMed->body.find("--model=gemini-4-argon-medium") != std::string::npos);

    server.stop();
    unsetenv("AIMON_IDE_BRAIN_DIR");
    unsetenv("AIMON_CLI_BRAIN_DIR");
    unsetenv("AIMON_WORKSPACE_ROOT");
    unsetenv("AIMON_AGY_BIN");
    unsetenv("AGY_LS_PORT");
    unsetenv("AGY_LS_CSRF_TOKEN");
    fs::remove_all(baseDir);
    std::cout << "[TestMobileGateway] testAgentPromptPreservesCascadeConfigAndModelM418 passed!" << std::endl;
}

int main() {
    std::cout << "=== Starting TestMobileGateway ===" << std::endl;
    std::string testDbPath = "/tmp/test_mobile_gw_telemetry_" + std::to_string(getpid()) + ".db";
    AgentTelemetryDb::getInstance().open(testDbPath);
    testPairingAndAuth();
    testSessionPersistenceAndPermissions();
    testRapidCheckTokenValidation();
    testApprovalLatching();
    testApprovalTimeout();
    testWebServerMobileAuth();
    testMultiRootBrainConversationDiscovery();
    testTranscriptJsonlParsingAndRapidCheck();
    testWorkspaceRecursiveTree();
    testAgentPromptNoFakeElizaAndRealBridge();
    testAgentPromptPreservesCascadeConfigAndModelM418();
    fs::remove(testDbPath);
    std::cout << "=== TestMobileGateway: ALL TESTS PASSED ===" << std::endl;
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
