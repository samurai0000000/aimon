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
#include <sqlite3.h>
#include "MobileGateway.hxx"
#include "AgentTelemetryDb.hxx"
#include "WebServer.hxx"
#include "StateStore.hxx"
#include "HistoryStore.hxx"

namespace fs = std::filesystem;
using namespace aimon;
using json = nlohmann::json;

static void pbEncodeVarint(std::vector<uint8_t>& buf, uint64_t val) {
    while (val >= 0x80) {
        buf.push_back(static_cast<uint8_t>((val & 0x7F) | 0x80));
        val >>= 7;
    }
    buf.push_back(static_cast<uint8_t>(val & 0x7F));
}

static void pbEncodeTag(std::vector<uint8_t>& buf, uint32_t fieldNum, uint32_t wireType) {
    pbEncodeVarint(buf, (static_cast<uint64_t>(fieldNum) << 3) | wireType);
}

static void pbEncodeVarintField(std::vector<uint8_t>& buf, uint32_t fieldNum, uint64_t val) {
    pbEncodeTag(buf, fieldNum, 0);
    pbEncodeVarint(buf, val);
}

static void pbEncodeLengthDelimitedField(std::vector<uint8_t>& buf, uint32_t fieldNum, const std::vector<uint8_t>& sub) {
    pbEncodeTag(buf, fieldNum, 2);
    pbEncodeVarint(buf, sub.size());
    buf.insert(buf.end(), sub.begin(), sub.end());
}

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

    auto resUnauthDevs = client.Get("/api/mobile/devices", spoofHeaders);
    assert(resUnauthDevs && resUnauthDevs->status == 401);

    auto resUnauthTree = client.Get("/api/workspace/tree", spoofHeaders);
    assert(resUnauthTree && resUnauthTree->status == 401);

    // 2. Invalid X-Mobile-Token MUST be rejected with 401
    httplib::Headers badTokenHeaders = {
        {"User-Agent", "aimon-mobile/2.0"},
        {"X-Mobile-Token", "invalid-token-value"}
    };
    auto resBadToken = client.Get("/api/mobile/devices", badTokenHeaders);
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

    auto resAuthDevs = client.Get("/api/mobile/devices", validHeaders);
    assert(resAuthDevs && resAuthDevs->status == 200);

    // 5. Authorization: Bearer <token> also succeeds
    httplib::Headers bearerHeaders = {
        {"Authorization", "Bearer " + validToken}
    };
    auto resBearerDevs = client.Get("/api/mobile/devices", bearerHeaders);
    assert(resBearerDevs && resBearerDevs->status == 200);

    // 6. Revoking the device invalidates its token immediately
    json revokeReq = {{"device_id", "ws-auth-dev"}};
    auto resRevoke = client.Post("/api/mobile/revoke", validHeaders, revokeReq.dump(), "application/json");
    assert(resRevoke && resRevoke->status == 200);

    auto resAfterRevoke = client.Get("/api/mobile/devices", validHeaders);
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
    GatewayConfig gwCfg;
    gwCfg.strictPresenceFdCheck = false;
    server.setGatewayConfig(gwCfg);
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
    // conv-38 should have active == true from presence lock when strictPresenceFdCheck is false, and title from USER_INPUT
    assert(convs[1].value("title", "") == "Prompt for conversation 38");
    assert(convs[1].value("active", false) == true);

    // With strictPresenceFdCheck = true (default), bare .lock without running process does not mark active
    gwCfg.strictPresenceFdCheck = true;
    server.setGatewayConfig(gwCfg);
    auto resStrict = client.Get("/api/agent/conversations", headers);
    assert(resStrict && resStrict->status == 200);
    json bodyStrict = json::parse(resStrict->body);
    assert(bodyStrict["conversations"][1].value("active", true) == false);

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

static void testSlashCommandsAndSessionNotifications() {
    std::cout << "[TestMobileGateway] Running testSlashCommandsAndSessionNotifications..." << std::endl;
    std::string baseDir = "/tmp/aimon_slash_notif_test_" + std::to_string(getpid());
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
    setenv("AIMON_NOTIFICATION_SETTLE_MS", "20", 1);

    // Seed an existing conversation before starting WebServer to verify startup seeding does not fire old alerts
    std::string seededCid = "conv-seeded-old";
    fs::path seededLogDir = fs::path(cliBrain) / seededCid / ".system_generated" / "logs";
    fs::create_directories(seededLogDir);
    {
        std::ofstream sf(seededLogDir / "transcript.jsonl");
        sf << "{\"step_index\":0,\"type\":\"USER_INPUT\",\"status\":\"DONE\",\"content\":\"<USER_REQUEST>\\nOld prompt\\n</USER_REQUEST>\"}\n";
        sf << "{\"step_index\":1,\"type\":\"PLANNER_RESPONSE\",\"status\":\"DONE\",\"content\":\"Old answer\",\"tool_calls\":[]}\n";
    }

    // Create mock agy binary supporting both `-p <slashCmd> --output-format json` and `agentapi send-message`
    std::string mockAgy = baseDir + "/mock_agy_slash.sh";
    {
        std::ofstream mf(mockAgy);
        mf << "#!/bin/sh\n"
           << "if [ \"$1\" = \"-p\" ]; then\n"
           << "  echo \"{\\\"status\\\":\\\"SUCCESS\\\",\\\"response\\\":\\\"Slash output for $2\\\",\\\"command\\\":{\\\"name\\\":\\\"cmd\\\"}}\"\n"
           << "  exit 0\n"
           << "fi\n"
           << "if [ \"$1\" = \"agentapi\" ] && [ \"$2\" = \"send-message\" ]; then\n"
           << "  echo \"{\\\"status\\\":\\\"sent\\\",\\\"recipient\\\":\\\"$3\\\",\\\"prompt\\\":\\\"$4\\\"}\"\n"
           << "  exit 0\n"
           << "fi\n"
           << "exit 1\n";
    }
    chmod(mockAgy.c_str(), 0755);
    setenv("AIMON_AGY_BIN", mockAgy.c_str(), 1);
    setenv("AGY_LS_PORT", "46797", 1);
    setenv("AGY_LS_CSRF_TOKEN", "slash-csrf", 1);

    MobileGateway &gw = MobileGateway::getInstance();
    assert(gw.init(tempMobileDb));
    gw.clearAgentEvents();
    std::string secret = gw.createPairingSecret(120);
    std::string validToken;
    assert(gw.verifyPairingCode(secret, "slash-notif-dev", "Test Phone", validToken));

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

    // 1. Verify seeded conversation did not generate any startup notification
    server.scanTranscriptsForNotifications(false);
    assert(gw.getRecentAgentEventsSince(0).empty());

    // 2. Verify /clear returns status="cleared" and conversationId="new"
    json clearReq = {{"text", "/clear"}, {"conversationId", "conv-1"}};
    auto resClear = client.Post("/api/agent/prompt", headers, clearReq.dump(), "application/json");
    assert(resClear && resClear->status == 200);
    json clearBody = json::parse(resClear->body);
    assert(clearBody.value("status", "") == "cleared");
    assert(clearBody.value("conversationId", "") == "new");

    // 3. Verify stateless print-mode slash commands (/help, /usage, /credits) return status="command_response"
    for (const std::string cmd : {"/help", "/usage", "/credits"}) {
        json cmdReq = {{"text", cmd}, {"conversationId", "conv-1"}};
        auto resCmd = client.Post("/api/agent/prompt", headers, cmdReq.dump(), "application/json");
        assert(resCmd && resCmd->status == 200);
        json cmdBody = json::parse(resCmd->body);
        assert(cmdBody.value("status", "") == "command_response");
        assert(cmdBody.value("command", "") == cmd);
        assert(cmdBody.value("response", "").find("Slash output for " + cmd) != std::string::npos);
    }

    // 4. Verify session slash commands (/compact, /grill-me) route to the conversation via agy/ConnectRPC
    json compactReq = {{"text", "/compact"}, {"conversationId", "conv-1"}};
    auto resCompact = client.Post("/api/agent/prompt", headers, compactReq.dump(), "application/json");
    assert(resCompact && resCompact->status == 200);
    json compactBody = json::parse(resCompact->body);
    assert(compactBody.value("status", "") == "dispatched");
    assert(compactBody.value("output", "").find("/compact") != std::string::npos);

    // 5. Verify agent_question detection when ask_question tool call is written to a live session
    std::string liveCid = "conv-live-notif-42";
    fs::path liveLogDir = fs::path(ideBrain) / liveCid / ".system_generated" / "logs";
    fs::create_directories(liveLogDir);
    fs::path liveTranscript = liveLogDir / "transcript.jsonl";
    {
        std::ofstream lf(liveTranscript);
        lf << "{\"step_index\":0,\"type\":\"USER_INPUT\",\"status\":\"DONE\",\"content\":\"<USER_REQUEST>\\nBuild mobile settings tab\\n</USER_REQUEST>\"}\n";
        lf << "{\"step_index\":1,\"type\":\"PLANNER_RESPONSE\",\"status\":\"DONE\",\"content\":\"\",\"tool_calls\":[{\"name\":\"ask_question\",\"args\":{\"questions\":[{\"question\":\"Which sessions should trigger notifications?\"}]}}]}\n";
    }
    server.scanTranscriptsForNotifications(false);
    auto eventsAfterQ = gw.getRecentAgentEventsSince(0);
    assert(eventsAfterQ.size() == 1);
    assert(eventsAfterQ[0].event == "agent_question");
    assert(eventsAfterQ[0].conversationId == liveCid);
    assert(eventsAfterQ[0].source == "ide");
    assert(eventsAfterQ[0].title == "Build mobile settings tab");
    assert(eventsAfterQ[0].snippet == "Which sessions should trigger notifications?");
    assert(eventsAfterQ[0].stepIndex == 1);
    uint64_t qSeq = eventsAfterQ[0].seq;

    // Repeated scan does not duplicate step 1
    server.scanTranscriptsForNotifications(false);
    assert(gw.getRecentAgentEventsSince(qSeq).empty());

    // 6. Verify intermediate background-task wait step does NOT fire a false prompt_answered
    {
        std::ofstream lf(liveTranscript, std::ios::app);
        lf << "{\"step_index\":2,\"type\":\"GENERIC\",\"status\":\"DONE\",\"content\":\"Tool is running as a background task with task id: task-123\"}\n";
        lf << "{\"step_index\":3,\"type\":\"PLANNER_RESPONSE\",\"status\":\"DONE\",\"content\":\"Waiting for background build to finish...\",\"tool_calls\":[]}\n";
    }
    server.scanTranscriptsForNotifications(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(35));
    server.scanTranscriptsForNotifications(false);
    assert(gw.getRecentAgentEventsSince(qSeq).empty());

    // 7. Verify terminal PLANNER_RESPONSE fires prompt_answered once settled
    {
        std::ofstream lf(liveTranscript, std::ios::app);
        lf << "{\"step_index\":4,\"type\":\"SYSTEM_MESSAGE\",\"status\":\"DONE\",\"content\":\"Background task completed\"}\n";
        lf << "{\"step_index\":5,\"type\":\"PLANNER_RESPONSE\",\"status\":\"DONE\",\"content\":\"All settings and notifications have been implemented and verified.\",\"tool_calls\":[]}\n";
    }
    server.scanTranscriptsForNotifications(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(35));
    server.scanTranscriptsForNotifications(false);
    auto eventsAfterAns = gw.getRecentAgentEventsSince(qSeq);
    assert(eventsAfterAns.size() == 1);
    assert(eventsAfterAns[0].event == "prompt_answered");
    assert(eventsAfterAns[0].conversationId == liveCid);
    assert(eventsAfterAns[0].stepIndex == 5);
    assert(eventsAfterAns[0].snippet.find("All settings and notifications have been implemented") != std::string::npos);

    // 8. Verify GET /api/mobile/notifications and POST /api/mobile/test-notification
    auto resNotifs = client.Get("/api/mobile/notifications?since=0", headers);
    assert(resNotifs && resNotifs->status == 200);
    json notifsBody = json::parse(resNotifs->body);
    assert(notifsBody.contains("events") && notifsBody["events"].size() == 2);

    json testNotifReq = {
        {"event", "prompt_answered"},
        {"conversationId", liveCid},
        {"title", "Manual Test"},
        {"snippet", "Testing sound and vibration"}
    };
    auto resTestNotif = client.Post("/api/mobile/test-notification", headers, testNotifReq.dump(), "application/json");
    assert(resTestNotif && resTestNotif->status == 200);
    json testNotifBody = json::parse(resTestNotif->body);
    assert(testNotifBody.value("status", "") == "ok");

    server.stop();
    unsetenv("AIMON_IDE_BRAIN_DIR");
    unsetenv("AIMON_CLI_BRAIN_DIR");
    unsetenv("AIMON_WORKSPACE_ROOT");
    unsetenv("AIMON_NOTIFICATION_SETTLE_MS");
    unsetenv("AIMON_AGY_BIN");
    unsetenv("AGY_LS_PORT");
    unsetenv("AGY_LS_CSRF_TOKEN");
    fs::remove_all(baseDir);
    std::cout << "[TestMobileGateway] testSlashCommandsAndSessionNotifications passed!" << std::endl;
}

static void testMultiProviderClaudeAndCursorSessionsAndNotifications() {
    std::cout << "[TestMobileGateway] Running testMultiProviderClaudeAndCursorSessionsAndNotifications..." << std::endl;
    std::string baseDir = "/tmp/aimon_multiprovider_" + std::to_string(getpid());
    std::string ideBrain = baseDir + "/ide_brain";
    std::string cliBrain = baseDir + "/cli_brain";
    std::string claudeProjects = baseDir + "/claude_projects";
    std::string cursorProjects = baseDir + "/cursor_projects";
    std::string tempMobileDb = baseDir + "/mobile_devices.json";
    fs::remove_all(baseDir);
    fs::create_directories(ideBrain);
    fs::create_directories(cliBrain);
    fs::create_directories(claudeProjects + "/-tmp-work-aimon");
    fs::create_directories(cursorProjects + "/tmp-work-aimon/agent-transcripts/cursor-sess-1/subagents");

    setenv("AIMON_IDE_BRAIN_DIR", ideBrain.c_str(), 1);
    setenv("AIMON_CLI_BRAIN_DIR", cliBrain.c_str(), 1);
    setenv("AIMON_CLAUDE_PROJECTS_DIR", claudeProjects.c_str(), 1);
    setenv("AIMON_CURSOR_PROJECTS_DIR", cursorProjects.c_str(), 1);
    setenv("AIMON_WORKSPACE_ROOT", baseDir.c_str(), 1);
    setenv("AIMON_NOTIFICATION_SETTLE_MS", "20", 1);

    // 1. Create an older Antigravity CLI session (last activity: 2026-10-24T01:00:00Z)
    fs::path agyLogDir = fs::path(cliBrain) / "agy-sess-1" / ".system_generated" / "logs";
    fs::create_directories(agyLogDir);
    {
        std::ofstream f(agyLogDir / "transcript.jsonl");
        f << "{\"step_index\":0,\"type\":\"USER_INPUT\",\"status\":\"DONE\",\"created_at\":\"2026-10-24T01:00:00Z\",\"content\":\"<USER_REQUEST>\\nAntigravity prompt 1\\n</USER_REQUEST>\"}\n";
    }

    // 2. Create a newer Claude Code session (last activity: 2026-10-24T03:00:00Z)
    fs::path claudeFile = fs::path(claudeProjects) / "-tmp-work-aimon" / "claude-sess-1.jsonl";
    {
        std::ofstream f(claudeFile);
        f << "{\"type\":\"user\",\"isSidechain\":false,\"cwd\":\"/tmp/work/aimon\",\"timestamp\":\"2026-10-24T02:55:00Z\",\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"<system-reminder>foo.cxx</system-reminder>\"},{\"type\":\"text\",\"text\":\"Analyze Claude Code session integration\"}]}}\n";
        f << "{\"type\":\"assistant\",\"isSidechain\":false,\"cwd\":\"/tmp/work/aimon\",\"timestamp\":\"2026-10-24T02:56:00Z\",\"message\":{\"role\":\"assistant\",\"model\":\"claude-sonnet-4-6\",\"content\":[{\"type\":\"thinking\",\"thinking\":\"Inspecting WebServer.cxx for Claude JSONL normalization\"},{\"type\":\"tool_use\",\"name\":\"Bash\",\"input\":{\"command\":\"ls -la\",\"description\":\"List workspace files\"}},{\"type\":\"text\",\"text\":\"I inspected the workspace files.\"}]}}\n";
        // Tool result turn (should NOT become a USER_INPUT step)
        f << "{\"type\":\"user\",\"isSidechain\":false,\"cwd\":\"/tmp/work/aimon\",\"timestamp\":\"2026-10-24T03:00:00Z\",\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"t1\",\"content\":\"total 42\"}]}}\n";
    }

    // Boundary: sidechain-only Claude file and empty file should be ignored
    {
        std::ofstream fSide(fs::path(claudeProjects) / "-tmp-work-aimon" / "claude-sidechain.jsonl");
        fSide << "{\"type\":\"user\",\"isSidechain\":true,\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"Subagent internal prompt\"}]}}\n";
        std::ofstream fEmpty(fs::path(claudeProjects) / "-tmp-work-aimon" / "claude-empty.jsonl");
    }

    // 3. Create the newest Cursor session (last activity: 2026-10-24T04:00:00Z) + a subagent file that must be excluded
    fs::path cursorFile = fs::path(cursorProjects) / "tmp-work-aimon" / "agent-transcripts" / "cursor-sess-1" / "cursor-sess-1.jsonl";
    {
        std::ofstream f(cursorFile);
        f << "{\"role\":\"user\",\"timestamp\":\"2026-10-24T03:58:00Z\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"<user_query>\\nFix Cursor session parser\\n</user_query>\"}]}}\n";
        f << "{\"role\":\"assistant\",\"timestamp\":\"2026-10-24T04:00:00Z\",\"message\":{\"model\":\"gpt-5\",\"content\":[{\"type\":\"thinking\",\"thinking\":\"Checking Cursor transcript format\"},{\"type\":\"tool_use\",\"name\":\"Read\",\"input\":{\"path\":\"src/WebServer.cxx\"}},{\"type\":\"text\",\"text\":\"Cursor parser updated cleanly.\"}]}}\n";

        std::ofstream fSub(fs::path(cursorProjects) / "tmp-work-aimon" / "agent-transcripts" / "cursor-sess-1" / "subagents" / "sub-1.jsonl");
        fSub << "{\"role\":\"user\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"Subagent task\"}]}}\n";

        // Create hooks/state/cursor-sess-1.json and chats/.../cursor-sess-1/store.db
        fs::create_directories(fs::path(cursorProjects).parent_path() / "hooks" / "state");
        std::ofstream fHook(fs::path(cursorProjects).parent_path() / "hooks" / "state" / "cursor-sess-1.json");
        fHook << "{\"session_id\":\"cursor-sess-1\",\"model\":\"auto-smart\",\"workspace\":\"/tmp/work/aimon\",\"turns\":2,\"step\":5}\n";
        fs::create_directories(fs::path(cursorProjects).parent_path() / "chats" / "test-hash" / "cursor-sess-1");
        std::ofstream fStore(fs::path(cursorProjects).parent_path() / "chats" / "test-hash" / "cursor-sess-1" / "store.db");
        fStore << "mock store";

        // Top-level duplicate directory for subagent sub-1
        fs::create_directories(fs::path(cursorProjects) / "tmp-work-aimon" / "agent-transcripts" / "sub-1");
        std::ofstream fSubTop(fs::path(cursorProjects) / "tmp-work-aimon" / "agent-transcripts" / "sub-1" / "sub-1.jsonl");
        fSubTop << "{\"role\":\"user\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"Subagent task top-level duplicate\"}]}}\n";

        // One-shot hook subagent sub-hook-0
        fs::create_directories(fs::path(cursorProjects) / "tmp-work-aimon" / "agent-transcripts" / "sub-hook-0");
        std::ofstream fSubHook(fs::path(cursorProjects) / "tmp-work-aimon" / "agent-transcripts" / "sub-hook-0" / "sub-hook-0.jsonl");
        fSubHook << "{\"role\":\"user\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"One-shot hook subagent\"}]}}\n";
        std::ofstream fHook0(fs::path(cursorProjects).parent_path() / "hooks" / "state" / "sub-hook-0.json");
        fHook0 << "{\"turns\":0,\"step\":10}\n";

        // <CONTEXT_SUMMARY> rollover orphan transcript
        fs::create_directories(fs::path(cursorProjects) / "tmp-work-aimon" / "agent-transcripts" / "cursor-rollover-orphan");
        std::ofstream fRoll(fs::path(cursorProjects) / "tmp-work-aimon" / "agent-transcripts" / "cursor-rollover-orphan" / "cursor-rollover-orphan.jsonl");
        fRoll << "{\"role\":\"user\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"<CONTEXT_SUMMARY>\\nDigest\\n</CONTEXT_SUMMARY>\\n<USER_REQUEST>/compact</USER_REQUEST>\\nPlease compact and summarize our conversation context in <CONTEXT_SUMMARY>\"}]}}\n";
    }

    // Set deterministic mtimes matching the activity order: agy (1000s ago), claude (500s ago), cursor (100s ago)
    auto nowFt = fs::file_time_type::clock::now();
    fs::last_write_time(agyLogDir / "transcript.jsonl", nowFt - std::chrono::seconds(1000));
    fs::last_write_time(claudeFile, nowFt - std::chrono::seconds(500));
    fs::last_write_time(cursorFile, nowFt - std::chrono::seconds(100));

    MobileGateway& gw = MobileGateway::getInstance();
    assert(gw.init(tempMobileDb));
    std::string secret = gw.createPairingSecret(120);
    std::string validToken;
    assert(gw.verifyPairingCode(secret, "multiprovider-dev", "Pixel 9 Pro", validToken));

    StateStore& stateStore = StateStore::getInstance();
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

    // Verify /api/agent/available lists antigravity, claude, and cursor
    auto resAvail = client.Get("/api/agent/available", headers);
    assert(resAvail && resAvail->status == 200);
    json availJson = json::parse(resAvail->body);
    assert(availJson.contains("agents") && availJson["agents"].size() >= 3);

    // Verify /api/agent/conversations discovers all 3 sessions in descending order of last activity
    // and excludes subagents & sidechains
    auto resConvs = client.Get("/api/agent/conversations", headers);
    assert(resConvs && resConvs->status == 200);
    json convsJson = json::parse(resConvs->body);
    assert(convsJson.contains("conversations") && convsJson["conversations"].is_array());
    auto convs = convsJson["conversations"];
    assert(convs.size() == 3);
    assert(convs[0].value("id", "") == "cursor-sess-1");
    assert(convs[0].value("source", "") == "cursor");
    assert(convs[0].value("model", "") == "auto-smart");
    assert(convs[0].value("title", "") == "Fix Cursor session parser");
    assert(convs[1].value("id", "") == "claude-sess-1");
    assert(convs[1].value("source", "") == "claude");
    assert(convs[1].value("title", "") == "Analyze Claude Code session integration");
    assert(convs[1].value("workspace", "") == "aimon");
    assert(convs[2].value("id", "") == "agy-sess-1");
    assert(convs[2].value("source", "") == "cli");
    assert(convs[0].value("timestamp", uint64_t(0)) >= convs[1].value("timestamp", uint64_t(0)));
    assert(convs[1].value("timestamp", uint64_t(0)) >= convs[2].value("timestamp", uint64_t(0)));

    // Verify GET /api/agent/transcript for Claude Code session
    auto resClaudeTrans = client.Get("/api/agent/transcript?id=claude-sess-1", headers);
    assert(resClaudeTrans && resClaudeTrans->status == 200);
    json claudeTrans = json::parse(resClaudeTrans->body);
    assert(claudeTrans.value("source", "") == "claude");
    assert(claudeTrans["steps"].size() == 2); // tool_result-only user turn is skipped
    assert(claudeTrans["steps"][0].value("type", "") == "USER_INPUT");
    assert(claudeTrans["steps"][0].value("text", "") == "Analyze Claude Code session integration");
    assert(claudeTrans["steps"][1].value("type", "") == "PLANNER_RESPONSE");
    assert(claudeTrans["steps"][1].value("thinking", "") == "Inspecting WebServer.cxx for Claude JSONL normalization");
    assert(claudeTrans["steps"][1].value("text", "") == "I inspected the workspace files.");
    assert(claudeTrans["steps"][1]["tool_calls"].size() == 1);
    assert(claudeTrans["steps"][1]["tool_calls"][0].value("name", "") == "Bash");
    assert(claudeTrans["steps"][1]["tool_calls"][0].value("command", "") == "ls -la");

    // Verify GET /api/agent/transcript for Cursor session
    auto resCursorTrans = client.Get("/api/agent/transcript?id=cursor-sess-1", headers);
    assert(resCursorTrans && resCursorTrans->status == 200);
    json cursorTrans = json::parse(resCursorTrans->body);
    assert(cursorTrans.value("source", "") == "cursor");
    assert(cursorTrans.value("model", "") == "auto-smart");
    assert(cursorTrans.value("workspace", "") == "aimon");
    assert(cursorTrans["steps"].size() == 2);
    assert(cursorTrans["steps"][0].value("type", "") == "USER_INPUT");
    assert(cursorTrans["steps"][0].value("text", "") == "Fix Cursor session parser");
    assert(cursorTrans["steps"][1].value("type", "") == "PLANNER_RESPONSE");
    assert(cursorTrans["steps"][1].value("text", "") == "Cursor parser updated cleanly.");
    assert(cursorTrans["steps"][1]["tool_calls"].size() == 1);
    assert(cursorTrans["steps"][1]["tool_calls"][0].value("name", "") == "Read");

    // Fault path: non-existent Claude binary returns 503
    setenv("AIMON_CLAUDE_BIN", (baseDir + "/missing_claude").c_str(), 1);
    json faultPrompt = {{"prompt", "Hello Claude"}, {"source", "claude"}, {"conversationId", "claude-sess-1"}};
    auto resFault = client.Post("/api/agent/prompt", headers, faultPrompt.dump(), "application/json");
    assert(resFault && resFault->status == 503);

    // Nominal prompt dispatch & autoApprove flag verification for Claude and Cursor
    std::string mockClaudeBin = baseDir + "/mock_claude.sh";
    std::string claudeArgsLog = baseDir + "/claude_args.txt";
    {
        std::ofstream mf(mockClaudeBin);
        mf << "#!/bin/sh\n";
        mf << "echo \"$@\" > \"" << claudeArgsLog << "\"\n";
        mf << "printf '{\"type\":\"result\",\"result\":\"Claude reply OK\",\"session_id\":\"claude-sess-1\"}\\n'\n";
    }
    chmod(mockClaudeBin.c_str(), 0755);
    setenv("AIMON_CLAUDE_BIN", mockClaudeBin.c_str(), 1);

    std::string mockCursorBin = baseDir + "/mock_cursor.sh";
    std::string cursorArgsLog = baseDir + "/cursor_args.txt";
    {
        std::ofstream mf(mockCursorBin);
        mf << "#!/bin/sh\n";
        mf << "echo \"$@\" > \"" << cursorArgsLog << "\"\n";
        mf << "printf '{\"type\":\"result\",\"result\":\"Cursor reply OK\"}\\n'\n";
    }
    chmod(mockCursorBin.c_str(), 0755);
    setenv("AIMON_CURSOR_BIN", mockCursorBin.c_str(), 1);

    json claudePromptReq = {
        {"prompt", "Continue Claude work"},
        {"source", "claude"},
        {"conversationId", "claude-sess-1"},
        {"model", "claude-sonnet-4-6"},
        {"autoApprove", true}
    };
    auto resClaudePrompt = client.Post("/api/agent/prompt", headers, claudePromptReq.dump(), "application/json");
    if (!resClaudePrompt || resClaudePrompt->status != 200) {
        std::cout << "DEBUG resClaudePrompt status=" << (resClaudePrompt ? resClaudePrompt->status : -1)
                  << " body=" << (resClaudePrompt ? resClaudePrompt->body : "") << std::endl;
    }
    assert(resClaudePrompt && resClaudePrompt->status == 200);
    {
        std::ifstream af(claudeArgsLog);
        std::string logged((std::istreambuf_iterator<char>(af)), std::istreambuf_iterator<char>());
        assert(logged.find("--resume claude-sess-1") != std::string::npos);
        assert(logged.find("--dangerously-skip-permissions") != std::string::npos);
        assert(logged.find("--model claude-sonnet-4-6") != std::string::npos);
    }

    json cursorPromptReq = {
        {"prompt", "Continue Cursor work"},
        {"source", "cursor"},
        {"conversationId", "cursor-sess-1"},
        {"model", "gpt-5"},
        {"autoApprove", true}
    };
    auto resCursorPrompt = client.Post("/api/agent/prompt", headers, cursorPromptReq.dump(), "application/json");
    assert(resCursorPrompt && resCursorPrompt->status == 200);
    {
        std::ifstream af(cursorArgsLog);
        std::string logged((std::istreambuf_iterator<char>(af)), std::istreambuf_iterator<char>());
        assert(logged.find("--resume cursor-sess-1") != std::string::npos);
        assert(logged.find("-f") != std::string::npos);
        assert(logged.find("--model gpt-5") != std::string::npos);
    }

    // Verify notifications for Claude Code (agent_question via AskUserQuestion) and Cursor (prompt_answered)
    uint64_t baseSeq = 0;
    auto initEvs = gw.getRecentAgentEventsSince(0);
    if (!initEvs.empty()) baseSeq = initEvs.back().seq;

    {
        std::ofstream f(claudeFile, std::ios::app);
        f << "{\"type\":\"assistant\",\"isSidechain\":false,\"cwd\":\"/tmp/work/aimon\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"name\":\"AskUserQuestion\",\"input\":{\"questions\":[{\"question\":\"Should Claude auto-approve tool calls?\"}]}}]}}\n";
        fs::last_write_time(claudeFile, fs::file_time_type::clock::now());
    }
    server.scanTranscriptsForNotifications(false);
    auto claudeEvs = gw.getRecentAgentEventsSince(baseSeq);
    assert(claudeEvs.size() == 1);
    assert(claudeEvs[0].event == "agent_question");
    assert(claudeEvs[0].source == "claude");
    assert(claudeEvs[0].conversationId == "claude-sess-1");
    assert(claudeEvs[0].snippet == "Should Claude auto-approve tool calls?");
    baseSeq = claudeEvs.back().seq;

    {
        std::ofstream f(cursorFile, std::ios::app);
        f << "{\"role\":\"user\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"Run tests\"}]}}\n";
        f << "{\"role\":\"assistant\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"All Cursor unit tests passed!\"}]}}\n";
        fs::last_write_time(cursorFile, fs::file_time_type::clock::now());
    }
    server.scanTranscriptsForNotifications(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(35));
    server.scanTranscriptsForNotifications(false);
    auto cursorEvs = gw.getRecentAgentEventsSince(baseSeq);
    assert(cursorEvs.size() == 1);
    assert(cursorEvs[0].event == "prompt_answered");
    assert(cursorEvs[0].source == "cursor");
    assert(cursorEvs[0].conversationId == "cursor-sess-1");
    assert(cursorEvs[0].snippet.find("All Cursor unit tests passed!") != std::string::npos);

    server.stop();
    unsetenv("AIMON_IDE_BRAIN_DIR");
    unsetenv("AIMON_CLI_BRAIN_DIR");
    unsetenv("AIMON_CLAUDE_PROJECTS_DIR");
    unsetenv("AIMON_CURSOR_PROJECTS_DIR");
    unsetenv("AIMON_WORKSPACE_ROOT");
    unsetenv("AIMON_NOTIFICATION_SETTLE_MS");
    unsetenv("AIMON_CLAUDE_BIN");
    unsetenv("AIMON_CURSOR_BIN");
    fs::remove_all(baseDir);
    std::cout << "[TestMobileGateway] testMultiProviderClaudeAndCursorSessionsAndNotifications passed!" << std::endl;
}

static void testSubagentFilterAskQuestionAndProviderSlashCommands() {
    std::cout << "[TestMobileGateway] Running testSubagentFilterAskQuestionAndProviderSlashCommands..." << std::endl;
    std::string baseDir = "/tmp/aimon_subagent_ux_" + std::to_string(getpid());
    std::string ideBrain = baseDir + "/ide_brain";
    std::string cliBrain = baseDir + "/cli_brain";
    std::string claudeProjects = baseDir + "/claude_projects";
    std::string cursorProjects = baseDir + "/cursor_projects";
    std::string tempMobileDb = baseDir + "/mobile_devices.json";
    fs::remove_all(baseDir);
    fs::create_directories(ideBrain);
    fs::create_directories(cliBrain);
    fs::create_directories(claudeProjects + "/-tmp-work-aimon");
    fs::create_directories(cursorProjects + "/tmp-work-aimon/agent-transcripts/cursor-sess-1");

    setenv("AIMON_IDE_BRAIN_DIR", ideBrain.c_str(), 1);
    setenv("AIMON_CLI_BRAIN_DIR", cliBrain.c_str(), 1);
    setenv("AIMON_CLAUDE_PROJECTS_DIR", claudeProjects.c_str(), 1);
    setenv("AIMON_CURSOR_PROJECTS_DIR", cursorProjects.c_str(), 1);
    setenv("AIMON_WORKSPACE_ROOT", baseDir.c_str(), 1);
    setenv("AIMON_NOTIFICATION_SETTLE_MS", "20", 1);

    // 1. Seed a main parent conversation
    std::string mainCid = "main-parent-1";
    fs::path mainLogDir = fs::path(cliBrain) / mainCid / ".system_generated" / "logs";
    fs::create_directories(mainLogDir);
    fs::path mainTranscript = mainLogDir / "transcript.jsonl";
    {
        std::ofstream f(mainTranscript);
        f << "{\"step_index\":0,\"type\":\"USER_INPUT\",\"status\":\"DONE\",\"created_at\":\"2026-10-24T05:00:00Z\",\"content\":\"<USER_REQUEST>\\nOrchestrate multi-envelope plan\\n</USER_REQUEST>\"}\n";
    }

    // 2. Seed a Cursor conversation for /compact rollover testing
    fs::path cursorFile = fs::path(cursorProjects) / "tmp-work-aimon" / "agent-transcripts" / "cursor-sess-1" / "cursor-sess-1.jsonl";
    {
        std::ofstream f(cursorFile);
        f << "{\"role\":\"user\",\"timestamp\":\"2026-10-24T05:01:00Z\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"<user_query>\\nImplement dynamic prompt box\\n</user_query>\"}]}}\n";
        f << "{\"role\":\"assistant\",\"timestamp\":\"2026-10-24T05:02:00Z\",\"message\":{\"model\":\"gpt-5\",\"content\":[{\"type\":\"text\",\"text\":\"Updated MessageInput.kt with dynamic heightIn.\"}]}}\n";
        fs::create_directories(fs::path(cursorProjects).parent_path() / "chats" / "test-hash" / "cursor-sess-1");
        std::ofstream fStore(fs::path(cursorProjects).parent_path() / "chats" / "test-hash" / "cursor-sess-1" / "store.db");
        fStore << "mock store";
    }

    // 3. Start a local mock LanguageServer to test HandleCascadeUserInteraction for waiting ask_question steps
    httplib::Server mockLs;
    std::mutex lsMutex;
    json lastInteractionReq;
    int interactionCallCount = 0;
    json lastCascadeMsgReq;
    int cascadeMsgCallCount = 0;

    mockLs.Post("/exa.language_server_pb.LanguageServerService/GetCascadeTrajectory",
                [&](const httplib::Request& req, httplib::Response& res) {
        auto body = json::parse(req.body, nullptr, false);
        std::string reqCid = body.value("cascadeId", "");
        if (reqCid != mainCid) {
            res.status = 404;
            res.set_content("{}", "application/json");
            return;
        }
        json traj = {
            {"trajectory", {
                {"trajectoryId", "traj-main-1"},
                {"steps", json::array({
                    {
                        {"type", "CORTEX_STEP_TYPE_USER_INPUT"},
                        {"status", "CORTEX_STEP_STATUS_DONE"},
                        {"userInput", {{"userResponse", "Orchestrate multi-envelope plan"}}}
                    },
                    {
                        {"type", "CORTEX_STEP_TYPE_ASK_QUESTION"},
                        {"status", "CORTEX_STEP_STATUS_WAITING"},
                        {"metadata", {
                            {"sourceTrajectoryStepInfo", {
                                {"trajectoryId", "traj-main-1"},
                                {"stepIndex", 5}
                            }}
                        }},
                        {"requestedInteraction", {
                            {"askQuestion", {
                                {"questions", json::array({
                                    {
                                        {"question", "Which option do you prefer?"},
                                        {"isMultiSelect", false},
                                        {"options", json::array({
                                            {{"id", "1"}, {"text", "(Recommended) Option Alpha"}},
                                            {{"id", "2"}, {"text", "Option Beta"}}
                                        })}
                                    }
                                })}
                            }}
                        }}
                    }
                })},
                {"executorMetadatas", json::array({
                    {
                        {"cascadeConfig", {
                            {"plannerConfig", {
                                {"planModel", "MODEL_PLACEHOLDER_M418"}
                            }}
                        }}
                    }
                })}
            }}
        };
        res.set_content(traj.dump(), "application/json");
    });

    mockLs.Post("/exa.language_server_pb.LanguageServerService/GetConversationMetadata",
                [&](const httplib::Request& req, httplib::Response& res) {
        auto body = json::parse(req.body, nullptr, false);
        std::string reqCid = body.value("conversationId", "");
        if (reqCid == mainCid) {
            res.set_content("{\"metadata\":{\"title\":\"Main Session\"}}", "application/json");
        } else {
            res.status = 404;
            res.set_content("{}", "application/json");
        }
    });

    mockLs.Post("/exa.language_server_pb.LanguageServerService/GetSlashCommands",
                [&](const httplib::Request& req, httplib::Response& res) {
        json slashResp = {
            {"commands", json::array({
                {
                    {"info", {
                        {"name", "compact"},
                        {"type", "SLASH_COMMAND_TYPE_SYSTEM"}
                    }}
                },
                {
                    {"info", {
                        {"name", "grill-me"},
                        {"type", "SLASH_COMMAND_TYPE_SYSTEM"},
                        {"modelFacingText", "<GRILL_ME>\nAsk questions one at a time.\n</GRILL_ME>"}
                    }}
                }
            })}
        };
        res.set_content(slashResp.dump(), "application/json");
    });

    mockLs.Post("/exa.language_server_pb.LanguageServerService/SendUserCascadeMessage",
                [&](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lk(lsMutex);
        lastCascadeMsgReq = json::parse(req.body, nullptr, false);
        ++cascadeMsgCallCount;
        res.set_content(json({{"cascadeId", mainCid}}).dump(), "application/json");
    });

    mockLs.Post("/exa.language_server_pb.LanguageServerService/HandleCascadeUserInteraction",
                [&](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lk(lsMutex);
        lastInteractionReq = json::parse(req.body, nullptr, false);
        ++interactionCallCount;
        res.set_content("{\"ok\":true}", "application/json");
    });

    int lsPort = mockLs.bind_to_any_port("127.0.0.1");
    assert(lsPort > 0);
    std::thread lsThread([&]() { mockLs.listen_after_bind(); });
    mockLs.wait_until_ready();

    std::string mockAgy = baseDir + "/mock_agy_ux.sh";
    {
        std::ofstream mf(mockAgy);
        mf << "#!/bin/sh\nexit 0\n";
    }
    chmod(mockAgy.c_str(), 0755);
    setenv("AIMON_AGY_BIN", mockAgy.c_str(), 1);
    setenv("AGY_LS_PORT", std::to_string(lsPort).c_str(), 1);
    setenv("AGY_LS_CSRF_TOKEN", "ux-csrf-token", 1);

    MobileGateway& gw = MobileGateway::getInstance();
    assert(gw.init(tempMobileDb));
    gw.clearAgentEvents();
    std::string secret = gw.createPairingSecret(120);
    std::string validToken;
    assert(gw.verifyPairingCode(secret, "subagent-ux-dev", "Test Phone", validToken));

    StateStore& stateStore = StateStore::getInstance();
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

    // --- Part 1: Subagent Exclusion in GET /api/agent/conversations & scanTranscriptsForNotifications ---
    // Create a subagent with SYSTEM_MESSAGE at step 0 AND a task.md file (plus a corrupted JSON line fault)
    std::string subCid1 = "subagent-with-task";
    fs::path subDir1 = fs::path(cliBrain) / subCid1;
    fs::create_directories(subDir1 / ".system_generated" / "logs");
    {
        std::ofstream tf(subDir1 / "task.md");
        tf << "# Subagent Envelope 1 Task\n";
        std::ofstream lf(subDir1 / ".system_generated" / "logs" / "transcript.jsonl");
        lf << "{corrupted json line that must be skipped\n";
        lf << "{\"step_index\":0,\"type\":\"SYSTEM_MESSAGE\",\"source\":\"SYSTEM\",\"status\":\"DONE\",\"content\":\"You are executing Envelope 1\"}\n";
        lf << "{\"step_index\":1,\"type\":\"PLANNER_RESPONSE\",\"status\":\"DONE\",\"content\":\"Subagent finished work\",\"tool_calls\":[]}\n";
    }

    // Create a second subagent marked via per-conversation cache/conversation_metadata.json
    std::string subCid2 = "subagent-meta-internal";
    fs::path subDir2 = fs::path(cliBrain) / subCid2;
    fs::create_directories(subDir2 / ".system_generated" / "logs");
    fs::create_directories(subDir2 / "cache");
    {
        std::ofstream mf(subDir2 / "cache" / "conversation_metadata.json");
        mf << "{\"is_internal\":true,\"parent_conversation_id\":\"main-parent-1\"}\n";
        std::ofstream lf(subDir2 / ".system_generated" / "logs" / "transcript.jsonl");
        lf << "{\"step_index\":0,\"type\":\"USER_INPUT\",\"status\":\"DONE\",\"content\":\"Internal task\"}\n";
        lf << "{\"step_index\":1,\"type\":\"PLANNER_RESPONSE\",\"status\":\"DONE\",\"content\":\"Internal subagent done\",\"tool_calls\":[]}\n";
    }

    // Create a third session with only PLANNER_RESPONSE (no USER_INPUT and no title)
    std::string subCid3 = "subagent-planner-only";
    fs::path subDir3 = fs::path(cliBrain) / subCid3;
    fs::create_directories(subDir3 / ".system_generated" / "logs");
    {
        std::ofstream lf(subDir3 / ".system_generated" / "logs" / "transcript.jsonl");
        lf << "{\"step_index\":0,\"type\":\"PLANNER_RESPONSE\",\"status\":\"DONE\",\"content\":\"Unprompted background output\",\"tool_calls\":[]}\n";
    }

    // Verify GET /api/agent/conversations excludes all three by default
    auto resConvsDefault = client.Get("/api/agent/conversations", headers);
    assert(resConvsDefault && resConvsDefault->status == 200);
    json convsDefault = json::parse(resConvsDefault->body)["conversations"];
    for (const auto& c : convsDefault) {
        std::string cidVal = c.value("id", "");
        assert(cidVal != subCid1);
        assert(cidVal != subCid2);
        assert(cidVal != subCid3);
    }

    // Verify include_subagents=true includes them
    auto resConvsAll = client.Get("/api/agent/conversations?include_subagents=true", headers);
    assert(resConvsAll && resConvsAll->status == 200);
    json convsAll = json::parse(resConvsAll->body)["conversations"];
    bool foundSub1 = false;
    bool foundSub2 = false;
    for (const auto& c : convsAll) {
        if (c.value("id", "") == subCid1) foundSub1 = true;
        if (c.value("id", "") == subCid2) foundSub2 = true;
    }
    assert(foundSub1 && foundSub2);

    // Verify scanTranscriptsForNotifications emits ZERO notifications for subagents even after settle delay
    server.scanTranscriptsForNotifications(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(35));
    server.scanTranscriptsForNotifications(false);
    assert(gw.getRecentAgentEventsSince(0).empty());

    // --- Part 2: Main Agent Pausing on "Created the following subagent:" ---
    {
        std::ofstream f(mainTranscript, std::ios::app);
        f << "{\"step_index\":1,\"type\":\"PLANNER_RESPONSE\",\"status\":\"DONE\",\"content\":\"Spawning Envelope 1 subagent.\",\"tool_calls\":[{\"name\":\"invoke_subagent\",\"args\":{\"Role\":\"Envelope 1 Executor\"}}]}\n";
        f << "{\"step_index\":2,\"type\":\"GENERIC\",\"status\":\"DONE\",\"content\":\"Created the following subagent:\\n- Conversation ID: subagent-with-task\"}\n";
        f << "{\"step_index\":3,\"type\":\"PLANNER_RESPONSE\",\"status\":\"DONE\",\"content\":\"Spawned subagent, waiting for completion.\",\"tool_calls\":[]}\n";
        fs::last_write_time(mainTranscript, fs::file_time_type::clock::now());
    }
    server.scanTranscriptsForNotifications(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(35));
    server.scanTranscriptsForNotifications(false);
    // Must NOT emit a premature prompt_answered while main agent is waiting for subagent!
    assert(gw.getRecentAgentEventsSince(0).empty());

    // --- Part 3: Stringified ask_question in transcript.jsonl & HandleCascadeUserInteraction ---
    {
        std::ofstream f(mainTranscript, std::ios::app);
        f << "{\"step_index\":4,\"type\":\"USER_INPUT\",\"status\":\"DONE\",\"content\":\"<USER_REQUEST>\\n/grill-me\\n</USER_REQUEST>\"}\n";
        f << "{\"step_index\":5,\"type\":\"PLANNER_RESPONSE\",\"status\":\"DONE\",\"thinking\":\"Formulating question\",\"content\":\"\",\"tool_calls\":[{\"name\":\"ask_question\",\"args\":{\"questions\":\"[{\\\"is_multi_select\\\":false,\\\"options\\\":[\\\"(Recommended) Option Alpha\\\",\\\"Option Beta\\\"],\\\"question\\\":\\\"Which option do you prefer?\\\"}]\"}}]}\n";
        fs::last_write_time(mainTranscript, fs::file_time_type::clock::now());
    }
    server.scanTranscriptsForNotifications(false);
    auto qEvents = gw.getRecentAgentEventsSince(0);
    assert(qEvents.size() == 1);
    assert(qEvents[0].event == "agent_question");
    assert(qEvents[0].conversationId == mainCid);
    assert(qEvents[0].snippet == "Which option do you prefer?");
    uint64_t lastSeq = qEvents.back().seq;

    // Verify GET /api/agent/transcript parses stringified args.questions into structured askQuestion
    auto resMainTrans = client.Get(("/api/agent/transcript?id=" + mainCid).c_str(), headers);
    assert(resMainTrans && resMainTrans->status == 200);
    json mainTransJson = json::parse(resMainTrans->body);
    bool foundAskQStep = false;
    for (const auto& st : mainTransJson["steps"]) {
        if (st.value("stepIndex", -1) == 5) {
            foundAskQStep = true;
            assert(st.contains("askQuestion") && st["askQuestion"].is_object());
            assert(st["askQuestion"].value("answered", true) == false);
            const auto& qs = st["askQuestion"]["questions"];
            assert(qs.is_array() && qs.size() == 1);
            assert(qs[0].value("question", "") == "Which option do you prefer?");
            assert(qs[0]["options"].size() == 2);
            assert(qs[0]["options"][0].get<std::string>() == "(Recommended) Option Alpha");
            assert(qs[0]["options"][1].get<std::string>() == "Option Beta");
            assert(st["tool_calls"][0]["arguments"]["questions"].is_array());
        }
    }
    assert(foundAskQStep);

    // Send structured questionResponses (with empty text!) to POST /api/agent/prompt and verify HandleCascadeUserInteraction
    json ansReq1 = {
        {"conversationId", mainCid},
        {"text", ""},
        {"questionResponses", json::array({
            {
                {"selectedOptions", json::array({"(Recommended) Option Alpha"})},
                {"writeInResponse", ""}
            }
        })}
    };
    auto resAns1 = client.Post("/api/agent/prompt", headers, ansReq1.dump(), "application/json");
    assert(resAns1 && resAns1->status == 200);
    json ansBody1 = json::parse(resAns1->body);
    assert(ansBody1.value("status", "") == "dispatched");
    assert(ansBody1.value("interactionHandled", false) == true);
    {
        std::lock_guard<std::mutex> lk(lsMutex);
        assert(interactionCallCount == 1);
        assert(lastInteractionReq.value("cascadeId", "") == mainCid);
        const auto& respArr = lastInteractionReq["interaction"]["askQuestion"]["responses"];
        assert(respArr.is_array() && respArr.size() == 1);
        assert(respArr[0]["selectedOptionIds"].size() == 1);
        assert(respArr[0]["selectedOptionIds"][0].get<std::string>() == "1");
        assert(lastInteractionReq["interaction"]["askQuestion"].value("cancelled", true) == false);
    }

    // Also test plain numeric text "2" from the prompt box when ask_question is waiting
    json ansReq2 = {
        {"conversationId", mainCid},
        {"text", "2"}
    };
    auto resAns2 = client.Post("/api/agent/prompt", headers, ansReq2.dump(), "application/json");
    assert(resAns2 && resAns2->status == 200);
    {
        std::lock_guard<std::mutex> lk(lsMutex);
        assert(interactionCallCount == 2);
        const auto& respArr = lastInteractionReq["interaction"]["askQuestion"]["responses"];
        assert(respArr[0]["selectedOptionIds"].size() == 1);
        assert(respArr[0]["selectedOptionIds"][0].get<std::string>() == "2");
    }

    // Append GENERIC answer step + final PLANNER_RESPONSE and verify askQuestion.answered == true & prompt_answered fires
    {
        std::ofstream f(mainTranscript, std::ios::app);
        f << "{\"step_index\":6,\"type\":\"GENERIC\",\"status\":\"DONE\",\"content\":\"Q1: Which option do you prefer?\\nA1: (Recommended) Option Alpha\"}\n";
        f << "{\"step_index\":7,\"type\":\"PLANNER_RESPONSE\",\"status\":\"DONE\",\"content\":\"All envelopes completed.\",\"tool_calls\":[]}\n";
        fs::last_write_time(mainTranscript, fs::file_time_type::clock::now());
    }
    auto resMainTransAfter = client.Get(("/api/agent/transcript?id=" + mainCid).c_str(), headers);
    assert(resMainTransAfter && resMainTransAfter->status == 200);
    json mainTransAfterJson = json::parse(resMainTransAfter->body);
    for (const auto& st : mainTransAfterJson["steps"]) {
        if (st.value("stepIndex", -1) == 5) {
            assert(st["askQuestion"].value("answered", false) == true);
            assert(!st["askQuestion"]["answers"].empty());
            assert(st["askQuestion"]["answers"][0].get<std::string>() == "(Recommended) Option Alpha");
        }
    }

    server.scanTranscriptsForNotifications(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(35));
    server.scanTranscriptsForNotifications(false);
    auto finalEvs = gw.getRecentAgentEventsSince(lastSeq);
    assert(finalEvs.size() == 1);
    assert(finalEvs[0].event == "prompt_answered");
    assert(finalEvs[0].conversationId == mainCid);
    assert(finalEvs[0].snippet == "All envelopes completed.");

    // --- Part 4: Provider Slash Commands (/clear, /compact, /status, /doctor, /models, /plan, /ask, /effort) ---
    std::string mockClaudeBin = baseDir + "/mock_claude_slash.sh";
    std::string claudeArgsLog = baseDir + "/claude_slash_args.txt";
    {
        std::ofstream mf(mockClaudeBin);
        mf << "#!/bin/sh\n";
        mf << "if [ \"$1\" = \"--version\" ]; then\n";
        mf << "  echo \"2.1.112 (Claude Code)\"\n";
        mf << "  exit 0\n";
        mf << "fi\n";
        mf << "printf '%s\\n' \"$@\" > \"" << claudeArgsLog << "\"\n";
        mf << "printf '{\"type\":\"result\",\"result\":\"Claude compacted/executed OK\",\"session_id\":\"claude-compacted-99\"}\\n'\n";
    }
    chmod(mockClaudeBin.c_str(), 0755);
    setenv("AIMON_CLAUDE_BIN", mockClaudeBin.c_str(), 1);

    std::string mockCursorBin = baseDir + "/mock_cursor_slash.sh";
    std::string cursorArgsLog = baseDir + "/cursor_slash_args.txt";
    {
        std::ofstream mf(mockCursorBin);
        mf << "#!/bin/sh\n";
        mf << "if [ \"$1\" = \"--version\" ]; then\n";
        mf << "  echo \"cursor-agent 0.48.0\"\n";
        mf << "  exit 0\n";
        mf << "fi\n";
        mf << "printf '%s\\n' \"$@\" > \"" << cursorArgsLog << "\"\n";
        mf << "printf '{\"type\":\"result\",\"result\":\"Cursor checkpoint summary ready\",\"session_id\":\"cursor-compacted-88\"}\\n'\n";
    }
    chmod(mockCursorBin.c_str(), 0755);
    setenv("AIMON_CURSOR_BIN", mockCursorBin.c_str(), 1);

    // 4a. /clear on claude and cursor
    for (const std::string prov : {"claude", "cursor"}) {
        json reqClear = {{"text", "/clear"}, {"source", prov}, {"conversationId", prov + "-sess-1"}};
        auto resCl = client.Post("/api/agent/prompt", headers, reqClear.dump(), "application/json");
        assert(resCl && resCl->status == 200);
        json bCl = json::parse(resCl->body);
        assert(bCl.value("status", "") == "cleared");
        assert(bCl.value("conversationId", "") == "new");
        assert(bCl.value("source", "") == prov);
    }

    // 4b. Boundary: /compact on "new" session returns command_response without invoking CLI
    {
        json reqCompNew = {{"text", "/compact"}, {"source", "cursor"}, {"conversationId", "new"}};
        auto resCompNew = client.Post("/api/agent/prompt", headers, reqCompNew.dump(), "application/json");
        assert(resCompNew && resCompNew->status == 200);
        json bCompNew = json::parse(resCompNew->body);
        assert(bCompNew.value("status", "") == "command_response");
        assert(bCompNew.value("response", "").find("No active conversation") != std::string::npos);
    }

    // 4c. /compact focus on tests on active cursor-sess-1 returns command_response preserving session history
    {
        json reqComp = {{"text", "/compact focus on tests"}, {"source", "cursor"}, {"conversationId", "cursor-sess-1"}};
        auto resComp = client.Post("/api/agent/prompt", headers, reqComp.dump(), "application/json");
        assert(resComp && resComp->status == 200);
        json bComp = json::parse(resComp->body);
        assert(bComp.value("status", "") == "command_response");
        assert(bComp.value("conversationId", "") == "cursor-sess-1");

        auto resTrans = client.Get("/api/agent/transcript?id=cursor-sess-1", headers);
        assert(resTrans && resTrans->status == 200);
        json tObj = json::parse(resTrans->body);
        assert(tObj["steps"].size() == 2);
        assert(tObj["steps"][0].value("type", "") == "USER_INPUT");
        assert(tObj["steps"][1].value("type", "") == "PLANNER_RESPONSE");
    }

    // 4d. Verify Claude Code native compact_boundary + isCompactSummary JSONL parsing
    {
        fs::path claudeCompFile = fs::path(claudeProjects) / "-tmp-work-aimon" / "claude-native-comp.jsonl";
        std::ofstream cf(claudeCompFile);
        cf << "{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"Old turn before compact\"}}\n";
        cf << "{\"type\":\"assistant\",\"message\":{\"role\":\"assistant\",\"content\":\"Old assistant turn\"}}\n";
        cf << "{\"type\":\"user\",\"entrypoint\":\"sdk-cli\",\"origin\":{\"kind\":\"task-notification\"},\"promptSource\":\"system\",\"queueTranscriptOnly\":true,\"message\":{\"role\":\"user\",\"content\":\"<task-notification>\\n<status>stopped</status>\\n</task-notification>\\nRead the output file to retrieve the result: /tmp/out\"}}\n";
        cf << "{\"type\":\"system\",\"subtype\":\"compact_boundary\",\"compactMetadata\":{\"preTokens\":96400,\"postTokens\":2300,\"durationMs\":26000}}\n";
        cf << "{\"type\":\"user\",\"isCompactSummary\":true,\"message\":{\"role\":\"user\",\"content\":\"This session is being continued from a previous conversation. Summary: Envelope 1 done.\"}}\n";
        cf << "{\"type\":\"user\",\"isMeta\":true,\"message\":{\"role\":\"user\",\"content\":\"<local-command-caveat>Caveat</local-command-caveat>\"}}\n";
        cf << "{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"<command-name>/compact</command-name>\\n<command-message>compact</command-message>\\n<command-args></command-args>\"}}\n";
        cf << "{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"<local-command-stdout>Compacted (ctrl+o to see full summary)</local-command-stdout>\"}}\n";
        cf.close();

        auto resCompTrans = client.Get("/api/agent/transcript?id=claude-native-comp", headers);
        assert(resCompTrans && resCompTrans->status == 200);
        json compTrans = json::parse(resCompTrans->body);
        assert(compTrans["steps"].size() == 3);
        assert(compTrans["steps"][0].value("type", "") == "USER_INPUT");
        assert(compTrans["steps"][0].value("text", "") == "Old turn before compact");
        assert(compTrans["steps"][1].value("type", "") == "PLANNER_RESPONSE");
        assert(compTrans["steps"][1].value("text", "") == "Old assistant turn");
        assert(compTrans["steps"][2].value("type", "") == "PLANNER_RESPONSE");
        assert(compTrans["steps"][2].value("text", "") == "2 steps summarized · context 96.4k → ~2.3k (2.3k summary) · took 26s");
        assert(compTrans["steps"][2].value("text", "").find("Summary: Envelope 1 done.") == std::string::npos);

        for (const auto& step : compTrans["steps"]) {
            std::string content = step.value("content", "") + " " + step.value("text", "");
            assert(content.find("<task-notification>") == std::string::npos);
            assert(content.find("<local-command-") == std::string::npos);
        }
    }

    // 4f. Cursor compaction summary extraction, XML tag stripping, duplicate user prompt deduplication, text + tool_use grouping
    {
        fs::path cursorBugDir = fs::path(cursorProjects) / "home-samurai-work-aimon" / "agent-transcripts" / "cursor-comp-tool-sess";
        fs::create_directories(cursorBugDir / "compaction");

        {
            std::ofstream seg(cursorBugDir / "compaction" / "segment_000.md");
            seg << "---\nworkspace: /tmp\n---\n";
            seg << "- Turns: 299\n- Verbose-render size estimate: 705,771 B\n";
            seg << "## Summary (curated by compaction step)\nCurated compaction summary point 1.\n";
            seg << "## Verbatim turns\n";
            seg << std::string(200000, 'X');
        }

        {
            std::ofstream jf(cursorBugDir / "cursor-comp-tool-sess.jsonl");
            jf << "{\"role\":\"user\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"<user_query>\\nCursor has committed his code changes\\n</user_query>\"}]}}\n";
            jf << "{\"role\":\"user\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"<available_subagent_types>\\nSubagent catalog\\n</available_subagent_types>\"}]}}\n";
            jf << "{\"role\":\"user\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"<timestamp>2026-10-24T12:00:00Z</timestamp>\\n<user_query>\\nCursor has committed his code changes\\n</user_query>\"}]}}\n";
            jf << "{\"role\":\"assistant\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"I will inspect the git commit.\"},{\"type\":\"tool_use\",\"name\":\"Shell\",\"input\":{\"command\":\"git status\",\"description\":\"Check git status\"}}]}}\n";
            jf << "{\"role\":\"assistant\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"All checks passed.\"}]}}\n";
        }

        auto resTrans = client.Get("/api/agent/transcript?id=cursor-comp-tool-sess", headers);
        assert(resTrans && resTrans->status == 200);
        json tObj = json::parse(resTrans->body);
        const auto& steps = tObj["steps"];
        assert(steps.size() == 4);

        // Step 0: compaction summary
        assert(steps[0].value("type", "") == "PLANNER_RESPONSE");
        std::string s0Text = steps[0].value("text", "");
        assert(s0Text.rfind("299 steps summarized · context ", 0) == 0);
        assert(s0Text.find("Curated compaction summary point 1.") == std::string::npos);
        assert(s0Text.find("Verbatim turns") == std::string::npos);
        assert(s0Text.find("XXXX") == std::string::npos);
        assert(steps[0]["text"].get<std::string>().size() < 13000);

        // Step 1: deduplicated USER_INPUT
        assert(steps[1].value("type", "") == "USER_INPUT");
        assert(steps[1].value("text", "") == "Cursor has committed his code changes");

        // Step 2: tool-calling assistant step
        assert(steps[2].value("type", "") == "PLANNER_RESPONSE");
        assert(steps[2].value("thinking", "") == "I will inspect the git commit.");
        assert(steps[2].value("text", "") == "");
        assert(steps[2]["step"].value("case", "") == "plannerResponse");
        assert(steps[2]["step"].value("value", "") == "I will inspect the git commit.");
        assert(steps[2]["tool_calls"].size() == 1);
        assert(steps[2]["tool_calls"][0].value("name", "") == "Shell");

        // Step 3: final assistant reply
        assert(steps[3].value("type", "") == "PLANNER_RESPONSE");
        assert(steps[3].value("text", "") == "All checks passed.");
        assert(steps[3]["step"].value("case", "") == "text");
        assert(steps[3]["step"].value("value", "") == "All checks passed.");

        // Incremental poll since=3
        auto resPoll = client.Get("/api/agent/transcript?id=cursor-comp-tool-sess&since=3", headers);
        assert(resPoll && resPoll->status == 200);
        json pollObj = json::parse(resPoll->body);
        assert(pollObj["steps"].empty());
        assert(pollObj["steps"].size() == 0);
    }

    // 4f-2. Cursor active subagent live merging, CallDynamicTool unwrapping, GetDynamicTools filtering, and synthetic prompt filtering
    {
        fs::path cursorLiveDir = fs::path(cursorProjects) / "home-samurai-work-aimon" / "agent-transcripts" / "cursor-live-sub-sess";
        fs::create_directories(cursorLiveDir / "subagents");
        fs::path parentJsonl = cursorLiveDir / "cursor-live-sub-sess.jsonl";
        {
            std::ofstream pf(parentJsonl);
            pf << "{\"role\":\"user\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"<timestamp>2026-10-25T01:55:22.405Z</timestamp>\\n<user_query>\\nstart-plan\\n</user_query>\"}]}}\n";
            pf << "{\"role\":\"user\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"Briefly inform the user about the task result and perform any follow-up actions (if needed). If there's no follow-ups needed, don't explicitly say that.\"}]}}\n";
            pf << "{\"role\":\"assistant\",\"message\":{\"content\":["
               << "{\"type\":\"text\",\"text\":\"I'll start by reading the plan-lifecycle skill and plan/plan_windows_kvm_pmuserenr.md, then inspect the collaboration-planning skill and prepare execution.\"},"
               << "{\"type\":\"tool_use\",\"name\":\"Read\",\"input\":{\"path\":\"/tmp/SKILL.md\"}},"
               << "{\"type\":\"tool_use\",\"name\":\"GetDynamicTools\",\"input\":{\"namespace\":\"cursor\",\"toolName\":\"Task\"}},"
               << "{\"type\":\"tool_use\",\"name\":\"CallDynamicTool\",\"input\":{\"toolName\":\"Task\",\"arguments\":{\"description\":\"Envelope 1 execution\"}}}"
               << "]}}\n";
        }
        auto baseTime = fs::file_time_type::clock::now();
        fs::last_write_time(parentJsonl, baseTime);

        fs::path subJsonl = cursorLiveDir / "subagents" / "9b75b6dd-e834-4fd0-aa79-c4659ae95f3a.jsonl";
        {
            std::ofstream sf(subJsonl);
            sf << "{\"role\":\"user\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"<timestamp>2026-10-25T01:58:18.732Z</timestamp>\\n<user_query>\\nExecute Envelope 1 CAN plumbing\\nDetails here\\n</user_query>\"}]}}\n";
            sf << "{\"role\":\"assistant\",\"message\":{\"content\":["
               << "{\"type\":\"thinking\",\"thinking\":\"Checking target files\"},"
               << "{\"type\":\"text\",\"text\":\"**Envelope state: Accepted.** All gates passed.\"},"
               << "{\"type\":\"tool_use\",\"name\":\"UpdateCurrentStep\",\"input\":{\"completed_subtitle\":\"Verified E1\",\"final_summary\":\"Done\"}}"
               << "]}}\n";
        }
        fs::last_write_time(subJsonl, baseTime + std::chrono::seconds(10));

        auto resLive = client.Get("/api/agent/transcript?id=cursor-live-sub-sess", headers);
        assert(resLive && resLive->status == 200);
        json liveObj = json::parse(resLive->body);
        const auto& lSteps = liveObj["steps"];
        // Step 0: USER_INPUT ("start-plan") — synthetic background prompt filtered out
        // Step 1: PLANNER_RESPONSE text bubble ("I'll start by reading...")
        // Step 2: PLANNER_RESPONSE tool_calls (Read + unwrapped Task; GetDynamicTools filtered out)
        // Step 3: Subagent banner ("🤖 **Subagent (9b75b6dd)** — Execute Envelope 1 CAN plumbing")
        // Step 4: Subagent completion text bubble ("**Envelope state: Accepted.** All gates passed.")
        assert(lSteps.size() == 5);
        assert(lSteps[0].value("type", "") == "USER_INPUT");
        assert(lSteps[0].value("text", "") == "start-plan");
        assert(lSteps[0].value("createdAt", "") == "2026-10-25T01:55:22.405Z");

        assert(lSteps[1].value("type", "") == "PLANNER_RESPONSE");
        assert(lSteps[1]["step"].value("case", "") == "text");
        assert(lSteps[1].value("text", "").find("I'll start by reading") != std::string::npos);
        assert(lSteps[1]["tool_calls"].empty());

        assert(lSteps[2].value("type", "") == "PLANNER_RESPONSE");
        assert(lSteps[2]["tool_calls"].size() == 2);
        assert(lSteps[2]["tool_calls"][0].value("name", "") == "Read");
        assert(lSteps[2]["tool_calls"][1].value("name", "") == "Task");
        assert(lSteps[2]["tool_calls"][1].value("toolSummary", "") == "Envelope 1 execution");

        assert(lSteps[3].value("type", "") == "PLANNER_RESPONSE");
        assert(lSteps[3]["step"].value("case", "") == "text");
        assert(lSteps[3].value("text", "").find("Subagent (9b75b6dd)") != std::string::npos);
        assert(lSteps[3].value("text", "").find("Execute Envelope 1 CAN plumbing") != std::string::npos);

        assert(lSteps[4].value("type", "") == "PLANNER_RESPONSE");
        assert(lSteps[4]["step"].value("case", "") == "text");
        assert(lSteps[4].value("text", "") == "**Envelope state: Accepted.** All gates passed.");
        assert(lSteps[4]["tool_calls"].empty());
    }

    // 4f-3. Cursor completed turn subagent exclusion
    {
        fs::path cursorLiveDir = fs::path(cursorProjects) / "home-samurai-work-aimon" / "agent-transcripts" / "cursor-live-sub-sess";
        fs::path parentJsonl = cursorLiveDir / "cursor-live-sub-sess.jsonl";
        fs::path subJsonl = cursorLiveDir / "subagents" / "9b75b6dd-e834-4fd0-aa79-c4659ae95f3a.jsonl";
        {
            std::ofstream pf(parentJsonl, std::ios::app);
            pf << "{\"type\":\"turn_ended\",\"status\":\"success\"}\n";
            pf << "{\"role\":\"assistant\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"Turn complete. Plan execution accepted.\"}]}}\n";
        }
        auto postTime = fs::file_time_type::clock::now();
        fs::last_write_time(parentJsonl, postTime);
        fs::last_write_time(subJsonl, postTime + std::chrono::seconds(5));

        auto resComp = client.Get("/api/agent/transcript?id=cursor-live-sub-sess", headers);
        assert(resComp && resComp->status == 200);
        json compObj = json::parse(resComp->body);
        const auto& cSteps = compObj["steps"];
        assert(cSteps.size() == 4);
        assert(cSteps[3].value("type", "") == "PLANNER_RESPONSE");
        assert(cSteps[3].value("text", "") == "Turn complete. Plan execution accepted.");
        for (const auto& st : cSteps) {
            assert(!st.value("isSubagentStep", false));
            assert(st.value("text", "").find("Subagent (9b75b6dd)") == std::string::npos);
        }
    }

    // 4f-4. Turn-1 Active Main Cursor Session Visibility
    {
        fs::path t1Dir = fs::path(cursorProjects) / "home-samurai-work-aimon" / "agent-transcripts" / "cursor-turn1-main";
        fs::create_directories(t1Dir);
        fs::path t1Jsonl = t1Dir / "cursor-turn1-main.jsonl";
        {
            std::ofstream tf(t1Jsonl);
            tf << "{\"role\":\"user\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"Implement feature X\"}]}}\n";
            tf << "{\"role\":\"assistant\",\"message\":{\"content\":[{\"type\":\"tool_use\",\"name\":\"Read\",\"input\":{\"path\":\"src/Foo.cxx\"}}]}}\n";
        }
        fs::path hookStateDir = fs::path(cursorProjects).parent_path() / "hooks" / "state";
        fs::create_directories(hookStateDir);
        {
            std::ofstream hf(hookStateDir / "cursor-turn1-main.json");
            hf << "{\"session_id\":\"cursor-turn1-main\",\"model\":\"claude-3-7-sonnet\",\"workspace\":\"/tmp/work/aimon\",\"turns\":0,\"step\":4}\n";
        }

        auto resConvs = client.Get("/api/agent/conversations", headers);
        assert(resConvs && resConvs->status == 200);
        json cObj = json::parse(resConvs->body);
        bool foundT1 = false;
        bool foundHook0 = false;
        for (const auto& item : cObj["conversations"]) {
            std::string id = item.value("id", item.value("conversationId", ""));
            if (id == "cursor-turn1-main") {
                foundT1 = true;
            }
            if (id == "sub-hook-0") {
                foundHook0 = true;
            }
        }
        assert(foundT1);
        assert(!foundHook0);
    }

    // 4f-5. Claude parentUuid Dead-Branch Filtering + Active Subagent Tail-Merging
    {
        fs::path claudeDagDir = fs::path(claudeProjects) / "-tmp-work-aimon";
        fs::create_directories(claudeDagDir / "claude-dag-sub" / "subagents");
        fs::path claudeDagFile = claudeDagDir / "claude-dag-sub.jsonl";
        {
            std::ofstream cf(claudeDagFile);
            cf << "{\"type\":\"user\",\"uuid\":\"u1\",\"parentUuid\":null,\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"Turn 1 user prompt\"}]}}\n";
            cf << "{\"type\":\"assistant\",\"uuid\":\"a1\",\"parentUuid\":\"u1\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"Turn 1 assistant reply\"}]}}\n";
            cf << "{\"type\":\"user\",\"uuid\":\"u_dead\",\"parentUuid\":\"a1\",\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"Dead branch prompt\"}]}}\n";
            cf << "{\"type\":\"assistant\",\"uuid\":\"a_dead\",\"parentUuid\":\"u_dead\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"Dead branch reply\"}]}}\n";
            cf << "{\"type\":\"user\",\"uuid\":\"u2\",\"parentUuid\":\"a1\",\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"Run subagent task\"}]}}\n";
            cf << "{\"type\":\"assistant\",\"uuid\":\"a2\",\"parentUuid\":\"u2\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"toolu_sub_1\",\"name\":\"Agent\",\"input\":{\"prompt\":\"Subagent doing work\"}}]}}\n";
        }

        fs::path claudeSubFile = claudeDagDir / "claude-dag-sub" / "subagents" / "agent-sub1.jsonl";
        {
            std::ofstream sf(claudeSubFile);
            sf << "{\"type\":\"user\",\"uuid\":\"sub_u1\",\"parentUuid\":null,\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"Subagent internal prompt\"}]}}\n";
            sf << "{\"type\":\"assistant\",\"uuid\":\"sub_a1\",\"parentUuid\":\"sub_u1\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"Subagent work done successfully\"}]}}\n";
        }

        auto cBaseTime = fs::file_time_type::clock::now();
        fs::last_write_time(claudeDagFile, cBaseTime);
        fs::last_write_time(claudeSubFile, cBaseTime + std::chrono::seconds(5));

        auto resDag = client.Get("/api/agent/transcript?id=claude-dag-sub", headers);
        assert(resDag && resDag->status == 200);
        json dagObj = json::parse(resDag->body);
        const auto& dSteps = dagObj["steps"];
        // Dead branch must be excluded, active Turn 1 + Turn 2 + live subagent must be present
        assert(dSteps.size() == 6);
        assert(dSteps[0].value("type", "") == "USER_INPUT");
        assert(dSteps[0].value("text", "") == "Turn 1 user prompt");
        assert(dSteps[1].value("type", "") == "PLANNER_RESPONSE");
        assert(dSteps[1].value("text", "") == "Turn 1 assistant reply");
        assert(dSteps[2].value("type", "") == "USER_INPUT");
        assert(dSteps[2].value("text", "") == "Run subagent task");
        assert(dSteps[3].value("type", "") == "PLANNER_RESPONSE");
        assert(dSteps[3]["tool_calls"].size() == 1);
        assert(dSteps[3]["tool_calls"][0].value("name", "") == "Agent");

        assert(dSteps[4].value("type", "") == "PLANNER_RESPONSE");
        assert(dSteps[4].value("isSubagentStep", false));
        assert(dSteps[4].value("text", "").find("Subagent (sub1)") != std::string::npos);
        assert(dSteps[5].value("type", "") == "PLANNER_RESPONSE");
        assert(dSteps[5].value("isSubagentStep", false));
        assert(dSteps[5].value("text", "") == "Subagent work done successfully");

        for (const auto& st : dSteps) {
            assert(st.value("text", "").find("Dead branch") == std::string::npos);
        }

        // Now append task-notification completing toolu_sub_1 and final assistant reply
        {
            std::ofstream cf(claudeDagFile, std::ios::app);
            cf << "{\"type\":\"user\",\"uuid\":\"u_notif\",\"parentUuid\":\"a2\",\"message\":{\"role\":\"user\",\"content\":\"<task-notification>\\n<tool-use-id>toolu_sub_1</tool-use-id>\\n<status>completed</status>\\n</task-notification>\"}}\n";
            cf << "{\"type\":\"assistant\",\"uuid\":\"a_final\",\"parentUuid\":\"u_notif\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"Subagent finished, parent turn complete.\"}]}}\n";
        }
        auto cPostTime = fs::file_time_type::clock::now();
        fs::last_write_time(claudeDagFile, cPostTime);
        fs::last_write_time(claudeSubFile, cPostTime + std::chrono::seconds(5));

        auto resDagDone = client.Get("/api/agent/transcript?id=claude-dag-sub", headers);
        assert(resDagDone && resDagDone->status == 200);
        json dagDoneObj = json::parse(resDagDone->body);
        const auto& ddSteps = dagDoneObj["steps"];
        // Subagent steps must be excluded now that toolu_sub_1 completed
        assert(ddSteps.size() == 5);
        assert(ddSteps[4].value("type", "") == "PLANNER_RESPONSE");
        assert(ddSteps[4].value("text", "") == "Subagent finished, parent turn complete.");
        for (const auto& st : ddSteps) {
            assert(!st.value("isSubagentStep", false));
            assert(st.value("text", "").find("Subagent (sub1)") == std::string::npos);
            assert(st.value("text", "").find("Dead branch") == std::string::npos);
        }
    }

    // 4e. /plan, /ask, /effort, /status, /doctor, /models
    {
        json reqPlanClaude = {{"text", "/plan Design auth flow"}, {"source", "claude"}, {"conversationId", "claude-sess-1"}};
        auto resPlanClaude = client.Post("/api/agent/prompt", headers, reqPlanClaude.dump(), "application/json");
        assert(resPlanClaude && resPlanClaude->status == 200);
        std::ifstream af(claudeArgsLog);
        std::string logged((std::istreambuf_iterator<char>(af)), std::istreambuf_iterator<char>());
        assert(logged.find("--permission-mode\nplan") != std::string::npos);
        assert(logged.find("Design auth flow") != std::string::npos);
    }
    {
        json reqPlanCursor = {{"text", "/plan Design auth flow"}, {"source", "cursor"}, {"conversationId", "cursor-sess-1"}};
        auto resPlanCursor = client.Post("/api/agent/prompt", headers, reqPlanCursor.dump(), "application/json");
        assert(resPlanCursor && resPlanCursor->status == 200);
        std::ifstream af(cursorArgsLog);
        std::string logged((std::istreambuf_iterator<char>(af)), std::istreambuf_iterator<char>());
        assert(logged.find("--mode\nplan") != std::string::npos);
        assert(logged.find("Design auth flow") != std::string::npos);
    }
    {
        json reqAskCursor = {{"text", "/ask How does auth work?"}, {"source", "cursor"}, {"conversationId", "cursor-sess-1"}};
        auto resAskCursor = client.Post("/api/agent/prompt", headers, reqAskCursor.dump(), "application/json");
        assert(resAskCursor && resAskCursor->status == 200);
        std::ifstream af(cursorArgsLog);
        std::string logged((std::istreambuf_iterator<char>(af)), std::istreambuf_iterator<char>());
        assert(logged.find("--mode\nask") != std::string::npos);
        assert(logged.find("How does auth work?") != std::string::npos);
    }
    {
        json reqEffortClaude = {{"text", "/effort high Refactor parser"}, {"source", "claude"}, {"conversationId", "claude-sess-1"}};
        auto resEffortClaude = client.Post("/api/agent/prompt", headers, reqEffortClaude.dump(), "application/json");
        assert(resEffortClaude && resEffortClaude->status == 200);
        std::ifstream af(claudeArgsLog);
        std::string logged((std::istreambuf_iterator<char>(af)), std::istreambuf_iterator<char>());
        assert(logged.find("--effort\nhigh") != std::string::npos);
        assert(logged.find("Refactor parser") != std::string::npos);
    }
    for (const std::string cmd : {"/status", "/doctor", "/models"}) {
        for (const std::string prov : {"claude", "cursor"}) {
            json reqCmd = {{"text", cmd}, {"source", prov}, {"conversationId", prov + "-sess-1"}};
            auto resCmd = client.Post("/api/agent/prompt", headers, reqCmd.dump(), "application/json");
            assert(resCmd && resCmd->status == 200);
            json bCmd = json::parse(resCmd->body);
            assert(bCmd.value("status", "") == "command_response");
            assert(bCmd.value("command", "") == cmd);
            assert(!bCmd.value("response", "").empty());
        }
    }

    // 4g. Test Antigravity slash command dispatch (/compact) and CHECKPOINT step handling
    {
        {
            std::lock_guard<std::mutex> lk(lsMutex);
            cascadeMsgCallCount = 0;
            lastCascadeMsgReq = nullptr;
        }
        json promptReq = {{"conversationId", mainCid}, {"text", "/compact"}};
        auto res = client.Post("/api/agent/prompt", headers, promptReq.dump(), "application/json");
        assert(res && res->status == 200);
        json b = json::parse(res->body);
        assert(b.value("status", "") == "dispatched");
        {
            std::lock_guard<std::mutex> lk(lsMutex);
            assert(cascadeMsgCallCount == 1);
            assert(lastCascadeMsgReq.contains("items") && lastCascadeMsgReq["items"].is_array());
            assert(lastCascadeMsgReq["items"].size() == 2);
            assert(lastCascadeMsgReq["items"][0]["item"]["slashCommand"]["info"]["name"] == "compact");
            assert(lastCascadeMsgReq["items"][0]["item"]["slashCommand"]["info"]["type"] == "SLASH_COMMAND_TYPE_SYSTEM");
            assert(lastCascadeMsgReq["items"][1]["text"] == "");
        }
    }
    {
        json promptReq = {{"conversationId", mainCid}, {"text", "/compact focus on memory"}};
        auto res = client.Post("/api/agent/prompt", headers, promptReq.dump(), "application/json");
        assert(res && res->status == 200);
        json b = json::parse(res->body);
        assert(b.value("status", "") == "dispatched");
        {
            std::lock_guard<std::mutex> lk(lsMutex);
            assert(cascadeMsgCallCount == 2);
            assert(lastCascadeMsgReq.contains("items") && lastCascadeMsgReq["items"].is_array());
            assert(lastCascadeMsgReq["items"].size() == 2);
            assert(lastCascadeMsgReq["items"][0]["item"]["slashCommand"]["info"]["name"] == "compact");
            assert(lastCascadeMsgReq["items"][1]["text"] == " focus on memory");
        }
    }

    // Append CHECKPOINT step (step_index: 8) to mainTranscript
    {
        std::ofstream tf(mainTranscript, std::ios::app);
        tf << "{\"step_index\":8,\"source\":\"SYSTEM\",\"type\":\"CHECKPOINT\",\"status\":\"DONE\",\"content\":\"# Resuming from a compaction\\n\\n<summary>\\n1. **Task Overview**\\n   - Compacted Antigravity context.\\n</summary>\\n\"}\n";
        fs::last_write_time(mainTranscript, fs::file_time_type::clock::now());
    }

    // Fetch GET /api/agent/transcript?id=<mainCid> (transcript.jsonl fallback without .db)
    {
        auto tRes = client.Get(("/api/agent/transcript?id=" + mainCid).c_str(), headers);
        assert(tRes && tRes->status == 200);
        json tj = json::parse(tRes->body);
        assert(tj.contains("steps") && tj["steps"].is_array());
        assert(!tj["steps"].empty());
        const auto& lastStep = tj["steps"].back();
        assert(lastStep.value("stepIndex", -1) == 8);
        assert(lastStep.value("type", "") == "PLANNER_RESPONSE");
        std::string txt = lastStep.value("text", "");
        assert(txt.find("steps summarized") != std::string::npos);
        assert(txt.find("summary") != std::string::npos);
        assert(txt.find("Compacted Antigravity context.") == std::string::npos);
    }

    // Run server.scanTranscriptsForNotifications(false) (with a 35ms sleep + second scan) and verify prompt_answered event
    gw.clearAgentEvents();
    server.scanTranscriptsForNotifications(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(35));
    server.scanTranscriptsForNotifications(false);
    {
        auto events = gw.getRecentAgentEventsSince(0);
        bool foundCompactedEvent = false;
        for (const auto& ev : events) {
            if (ev.conversationId == mainCid && ev.event == "prompt_answered") {
                assert(ev.snippet == "Context compacted.");
                assert(ev.stepIndex == 8);
                foundCompactedEvent = true;
                break;
            }
        }
        assert(foundCompactedEvent);
    }

    // Next, create SQLite database <cliRootDir>/conversations/<mainCid>.db and qualify protobuf checkpoint decoding
    {
        fs::path cliRootDir = fs::path(cliBrain).parent_path();
        fs::path convDbDir = cliRootDir / "conversations";
        fs::create_directories(convDbDir);
        fs::path convDbPath = convDbDir / (mainCid + ".db");

        sqlite3* convDb = nullptr;
        assert(sqlite3_open_v2(convDbPath.c_str(), &convDb, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) == SQLITE_OK && convDb);
        char* zErr = nullptr;
        assert(sqlite3_exec(convDb, "CREATE TABLE IF NOT EXISTS gen_metadata (idx INTEGER PRIMARY KEY, data BLOB, size INTEGER);", nullptr, nullptr, &zErr) == SQLITE_OK);
        assert(sqlite3_exec(convDb, "CREATE TABLE IF NOT EXISTS steps (idx INTEGER PRIMARY KEY, step_type INTEGER, status INTEGER, has_subtrajectory INTEGER, metadata BLOB, error_details BLOB, permissions BLOB, task_details BLOB, render_info BLOB, step_payload BLOB, step_format INTEGER);", nullptr, nullptr, &zErr) == SQLITE_OK);

        auto makeTimestamp = [](int64_t sec, int32_t nanos) {
            std::vector<uint8_t> b;
            pbEncodeVarintField(b, 1, static_cast<uint64_t>(sec));
            if (nanos != 0) pbEncodeVarintField(b, 2, static_cast<uint64_t>(nanos));
            return b;
        };

        auto makeSection = [](uint64_t sType, uint64_t sTokens) {
            std::vector<uint8_t> s;
            pbEncodeVarintField(s, 2, sType);
            pbEncodeVarintField(s, 4, sTokens);
            return s;
        };

        // gen_metadata: overheadTokens = 16280 (System Prompt 11575 + Tools 4705)
        std::vector<uint8_t> genBlob;
        {
            auto s1 = makeSection(1, 11575);
            auto s2 = makeSection(2, 4705);

            std::vector<uint8_t> f3;
            pbEncodeLengthDelimitedField(f3, 1, s1);
            pbEncodeLengthDelimitedField(f3, 1, s2);

            std::vector<uint8_t> f10;
            pbEncodeLengthDelimitedField(f10, 3, f3);

            std::vector<uint8_t> f9;
            pbEncodeLengthDelimitedField(f9, 10, f10);

            std::vector<uint8_t> usage;
            pbEncodeVarintField(usage, 2, 16280);

            std::vector<uint8_t> cs;
            pbEncodeLengthDelimitedField(cs, 9, f9);
            pbEncodeLengthDelimitedField(cs, 4, usage);

            pbEncodeLengthDelimitedField(genBlob, 1, cs);
        }

        {
            sqlite3_stmt* gStmt = nullptr;
            assert(sqlite3_prepare_v2(convDb, "INSERT INTO gen_metadata (idx, data, size) VALUES (0, ?, ?)", -1, &gStmt, nullptr) == SQLITE_OK);
            sqlite3_bind_blob(gStmt, 1, genBlob.data(), static_cast<int>(genBlob.size()), SQLITE_STATIC);
            sqlite3_bind_int(gStmt, 2, static_cast<int>(genBlob.size()));
            assert(sqlite3_step(gStmt) == SQLITE_DONE);
            sqlite3_finalize(gStmt);
        }

        // Step 1206: step_type = 14 (USER_INPUT, created_at = 1792927140)
        {
            std::vector<uint8_t> meta14;
            pbEncodeLengthDelimitedField(meta14, 1, makeTimestamp(1792927140, 0));

            sqlite3_stmt* sStmt = nullptr;
            assert(sqlite3_prepare_v2(convDb, "INSERT INTO steps (idx, step_type, status, has_subtrajectory, metadata, step_payload, step_format) VALUES (1206, 14, 1, 0, ?, NULL, 0)", -1, &sStmt, nullptr) == SQLITE_OK);
            sqlite3_bind_blob(sStmt, 1, meta14.data(), static_cast<int>(meta14.size()), SQLITE_STATIC);
            assert(sqlite3_step(sStmt) == SQLITE_DONE);
            sqlite3_finalize(sStmt);
        }

        // Step 1207: step_type = 23 (CHECKPOINT)
        {
            std::vector<uint8_t> cp;
            pbEncodeVarintField(cp, 11, 1110);
            pbEncodeVarintField(cp, 12, 1207);

            std::vector<uint8_t> payload23;
            pbEncodeLengthDelimitedField(payload23, 30, cp);

            std::vector<uint8_t> usage;
            pbEncodeVarintField(usage, 2, 96423); // input_tokens
            pbEncodeVarintField(usage, 10, 2326); // response_output_tokens

            std::vector<uint8_t> chatModelMeta;
            pbEncodeLengthDelimitedField(chatModelMeta, 2, usage);

            std::vector<uint8_t> genMeta;
            pbEncodeLengthDelimitedField(genMeta, 2, chatModelMeta);

            std::vector<uint8_t> meta23;
            pbEncodeLengthDelimitedField(meta23, 1, makeTimestamp(1792927140, 0));
            pbEncodeLengthDelimitedField(meta23, 8, makeTimestamp(1792927166, 0));
            pbEncodeLengthDelimitedField(meta23, 28, genMeta);

            sqlite3_stmt* sStmt = nullptr;
            assert(sqlite3_prepare_v2(convDb, "INSERT INTO steps (idx, step_type, status, has_subtrajectory, metadata, step_payload, step_format) VALUES (1207, 23, 1, 0, ?, ?, 0)", -1, &sStmt, nullptr) == SQLITE_OK);
            sqlite3_bind_blob(sStmt, 1, meta23.data(), static_cast<int>(meta23.size()), SQLITE_STATIC);
            sqlite3_bind_blob(sStmt, 2, payload23.data(), static_cast<int>(payload23.size()), SQLITE_STATIC);
            assert(sqlite3_step(sStmt) == SQLITE_DONE);
            sqlite3_finalize(sStmt);
        }

        // Step 1208: step_type = 23 (Fault injection with truncated / corrupted protobuf)
        {
            const uint8_t corruptBytes[] = {0xff, 0xff, 0xff, 0xff, 0xfe, 0xed};
            sqlite3_stmt* sStmt = nullptr;
            assert(sqlite3_prepare_v2(convDb, "INSERT INTO steps (idx, step_type, status, has_subtrajectory, metadata, step_payload, step_format) VALUES (1208, 23, 1, 0, ?, ?, 0)", -1, &sStmt, nullptr) == SQLITE_OK);
            sqlite3_bind_blob(sStmt, 1, corruptBytes, sizeof(corruptBytes), SQLITE_STATIC);
            sqlite3_bind_blob(sStmt, 2, corruptBytes, sizeof(corruptBytes), SQLITE_STATIC);
            assert(sqlite3_step(sStmt) == SQLITE_DONE);
            sqlite3_finalize(sStmt);
        }

        sqlite3_close(convDb);
    }

    // Append CHECKPOINT step (step_index: 1207) to mainTranscript
    {
        std::ofstream tf(mainTranscript, std::ios::app);
        tf << "{\"step_index\":1207,\"source\":\"SYSTEM\",\"type\":\"CHECKPOINT\",\"status\":\"DONE\",\"content\":\"# Resuming from a compaction\\n\\n<summary>\\nCompacted.\\n</summary>\"}\n";
        fs::last_write_time(mainTranscript, fs::file_time_type::clock::now());
    }

    // Fetch GET /api/agent/transcript?id=<mainCid> and assert step 1207 exact text
    {
        auto tRes = client.Get(("/api/agent/transcript?id=" + mainCid).c_str(), headers);
        assert(tRes && tRes->status == 200);
        json tj = json::parse(tRes->body);
        assert(tj.contains("steps") && tj["steps"].is_array());
        assert(!tj["steps"].empty());
        const auto& lastStep = tj["steps"].back();
        assert(lastStep.value("stepIndex", -1) == 1207);
        assert(lastStep.value("type", "") == "PLANNER_RESPONSE");
        std::string txt = lastStep.value("text", "");
        assert(txt == "97 steps summarized · context 96.4k → ~18.6k (2.3k summary) · took 26s");
    }

    // Fault injection: append CHECKPOINT step (step_index: 1208) to mainTranscript
    {
        std::ofstream tf(mainTranscript, std::ios::app);
        tf << "{\"step_index\":1208,\"source\":\"SYSTEM\",\"type\":\"CHECKPOINT\",\"status\":\"DONE\",\"content\":\"Fault injected step\"}\n";
        fs::last_write_time(mainTranscript, fs::file_time_type::clock::now());
    }

    // Fetch GET /api/agent/transcript?id=<mainCid> and assert step 1208 graceful fallback
    {
        auto tRes = client.Get(("/api/agent/transcript?id=" + mainCid).c_str(), headers);
        assert(tRes && tRes->status == 200);
        json tj = json::parse(tRes->body);
        assert(tj.contains("steps") && tj["steps"].is_array());
        assert(!tj["steps"].empty());
        const auto& lastStep = tj["steps"].back();
        assert(lastStep.value("stepIndex", -1) == 1208);
        assert(lastStep.value("type", "") == "PLANNER_RESPONSE");
        std::string txt = lastStep.value("text", "");
        assert(txt.find("step summarized") != std::string::npos);
    }

    server.stop();
    mockLs.stop();
    if (lsThread.joinable()) lsThread.join();

    unsetenv("AIMON_IDE_BRAIN_DIR");
    unsetenv("AIMON_CLI_BRAIN_DIR");
    unsetenv("AIMON_CLAUDE_PROJECTS_DIR");
    unsetenv("AIMON_CURSOR_PROJECTS_DIR");
    unsetenv("AIMON_WORKSPACE_ROOT");
    unsetenv("AIMON_NOTIFICATION_SETTLE_MS");
    unsetenv("AIMON_AGY_BIN");
    unsetenv("AGY_LS_PORT");
    unsetenv("AGY_LS_CSRF_TOKEN");
    unsetenv("AIMON_CLAUDE_BIN");
    unsetenv("AIMON_CURSOR_BIN");
    fs::remove_all(baseDir);
    std::cout << "[TestMobileGateway] testSubagentFilterAskQuestionAndProviderSlashCommands passed!" << std::endl;
}

static void testOrphanSummaryFilteringAndZeroStepTerminalSession() {
    std::cout << "[TestMobileGateway] Running testOrphanSummaryFilteringAndZeroStepTerminalSession..." << std::endl;
    std::string baseDir = "/tmp/aimon_orphan_zero_step_test_" + std::to_string(getpid());
    fs::remove_all(baseDir);

    std::string ideBrain = baseDir + "/ide_brain";
    std::string cliRoot = baseDir + "/cli_root";
    std::string cliBrain = cliRoot + "/brain";
    std::string dbPath = cliRoot + "/conversation_summaries.db";
    std::string procsFile = baseDir + "/running_agy.json";
    fs::create_directories(ideBrain);
    fs::create_directories(cliBrain);

    // 1. Create conversation_summaries.db with an orphan row
    //    step_count = 4, title = "Asking for Identity and Time"
    //    NO transcript.jsonl, NO task.md, NO running agy process
    {
        sqlite3* db = nullptr;
        assert(sqlite3_open(dbPath.c_str(), &db) == SQLITE_OK);
        const char* createSql =
            "CREATE TABLE conversation_summaries ("
            "conversation_id TEXT PRIMARY KEY, "
            "title TEXT NOT NULL DEFAULT '', "
            "preview TEXT NOT NULL DEFAULT '', "
            "step_count INTEGER NOT NULL DEFAULT 0, "
            "last_modified_time TEXT NOT NULL, "
            "workspace_uris TEXT NOT NULL DEFAULT '[]', "
            "status TEXT NOT NULL DEFAULT '', "
            "parent_conversation_id TEXT NOT NULL DEFAULT '', "
            "nesting_depth INTEGER NOT NULL DEFAULT 0, "
            "agent_name TEXT NOT NULL DEFAULT ''"
            ");";
        char* err = nullptr;
        assert(sqlite3_exec(db, createSql, nullptr, nullptr, &err) == SQLITE_OK);

        const char* insertSql =
            "INSERT INTO conversation_summaries "
            "(conversation_id, title, preview, step_count, last_modified_time, workspace_uris, status) "
            "VALUES ('orphan-conv-db-only', 'Asking for Identity and Time', 'What time is it?', 4, "
            "'2026-10-24 15:00:00', '[\"file:///tmp/work/orphan_ws\"]', 'CASCADE_RUN_STATUS_COMPLETED');";
        assert(sqlite3_exec(db, insertSql, nullptr, nullptr, &err) == SQLITE_OK);
        sqlite3_close(db);
    }

    // 2. Setup running agy processes file for a 0-step session locked by running agy
    //    zero-step-terminal-conv in workspace /tmp/work/telemetry_project
    //    NO transcript.jsonl on disk
    std::string zeroStepCid = "zero-step-terminal-conv";
    fs::create_directories(cliBrain + "/" + zeroStepCid);
    {
        std::ofstream pf(procsFile);
        json pj = {
            {"processes", json::array({
                {
                    {"pid", 999123},
                    {"cwd", "/tmp/work/telemetry_project"},
                    {"lockedConversationId", zeroStepCid},
                    {"terminalPts", "pts/42"},
                    {"httpPort", 43210}
                }
            })}
        };
        pf << pj.dump(2);
    }

    // 3. Setup a task.md-only session with NO transcript.jsonl
    std::string taskMdCid = "task-md-only-conv";
    fs::create_directories(cliBrain + "/" + taskMdCid);
    {
        std::ofstream mf(cliBrain + "/" + taskMdCid + "/task.md");
        mf << "# Implementation Plan for ABC-80 Hardware Diagnostics\n\n1. Inspect ports\n2. Run loopback\n";
    }

    setenv("AIMON_IDE_BRAIN_DIR", ideBrain.c_str(), 1);
    setenv("AIMON_CLI_BRAIN_DIR", cliBrain.c_str(), 1);
    setenv("AIMON_AGY_PROCESSES_FILE", procsFile.c_str(), 1);
    setenv("AIMON_WORKSPACE_ROOT", baseDir.c_str(), 1);

    auto& gw = MobileGateway::getInstance();
    std::string pairingFile = baseDir + "/mobile_devices.json";
    assert(gw.init(pairingFile));
    std::string secret = gw.createPairingSecret(120);
    std::string validToken;
    assert(gw.pairDevice(secret, "dev-orphan-test", "Pixel 9 Pro", validToken));

    StateStore& stateStore = StateStore::getInstance();
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

    // Verify GET /api/agent/conversations
    auto resConvs = client.Get("/api/agent/conversations", headers);
    assert(resConvs && resConvs->status == 200);
    json convsJson = json::parse(resConvs->body);
    assert(convsJson.contains("conversations") && convsJson["conversations"].is_array());
    const auto& convList = convsJson["conversations"];

    bool foundOrphan = false;
    bool foundZeroStep = false;
    bool foundTaskMd = false;

    for (const auto& c : convList) {
        std::string cid = c.value("id", "");
        if (cid == "orphan-conv-db-only") {
            foundOrphan = true;
        } else if (cid == zeroStepCid) {
            foundZeroStep = true;
            assert(c.value("openInTerminal", false) == true);
            assert(c.value("title", "") == "New Session (telemetry_project)");
            assert(c.value("terminalPts", "") == "pts/42");
            assert(c.value("workspace", "") == "telemetry_project");
        } else if (cid == taskMdCid) {
            foundTaskMd = true;
        }
    }

    // a) Orphan row in conversation_summaries.db must be excluded
    assert(!foundOrphan);

    // b) 0-step conversation locked by running agy must be included
    assert(foundZeroStep);

    // c) task.md-only conversation must be included
    assert(foundTaskMd);

    // Verify GET /api/agent/transcript for zero-step terminal session: returns 200 with steps: []
    auto resZeroTrans = client.Get(("/api/agent/transcript?id=" + zeroStepCid).c_str(), headers);
    assert(resZeroTrans && resZeroTrans->status == 200);
    json zeroTrans = json::parse(resZeroTrans->body);
    assert(zeroTrans.value("openInTerminal", false) == true);
    assert(zeroTrans.value("terminalPts", "") == "pts/42");
    assert(zeroTrans.value("workspace", "") == "telemetry_project");
    assert(zeroTrans.value("stepCount", -1) == 0);
    assert(zeroTrans.contains("steps") && zeroTrans["steps"].is_array());
    assert(zeroTrans["steps"].empty());

    // Verify GET /api/agent/transcript for task.md-only session: returns 200 with 1 PLANNER_RESPONSE step
    auto resTaskTrans = client.Get(("/api/agent/transcript?id=" + taskMdCid).c_str(), headers);
    assert(resTaskTrans && resTaskTrans->status == 200);
    json taskTrans = json::parse(resTaskTrans->body);
    assert(taskTrans.value("stepCount", -1) == 1);
    assert(taskTrans.contains("steps") && taskTrans["steps"].size() == 1);
    assert(taskTrans["steps"][0].value("type", "") == "PLANNER_RESPONSE");
    std::string taskContent = taskTrans["steps"][0].value("text", "");
    assert(taskContent.find("Implementation Plan for ABC-80 Hardware Diagnostics") != std::string::npos);

    // Verify GET /api/agent/transcript for orphan session returns 404
    auto resOrphanTrans = client.Get("/api/agent/transcript?id=orphan-conv-db-only", headers);
    assert(resOrphanTrans && resOrphanTrans->status == 404);

    server.stop();

    unsetenv("AIMON_IDE_BRAIN_DIR");
    unsetenv("AIMON_CLI_BRAIN_DIR");
    unsetenv("AIMON_AGY_PROCESSES_FILE");
    unsetenv("AIMON_WORKSPACE_ROOT");
    fs::remove_all(baseDir);
    std::cout << "[TestMobileGateway] testOrphanSummaryFilteringAndZeroStepTerminalSession passed!" << std::endl;
}

static void testLiveScreenSessionRoutingAndIdeSessionProtection() {
    std::cout << "[TestMobileGateway] Running testLiveScreenSessionRoutingAndIdeSessionProtection..." << std::endl;
    std::string baseDir = "/tmp/aimon_live_screen_test_" + std::to_string(getpid());
    fs::remove_all(baseDir);

    std::string ideBrain = baseDir + "/ide_brain";
    std::string cliBrain = baseDir + "/cli_brain";
    std::string claudeProjects = baseDir + "/claude_projects";
    std::string cursorProjects = baseDir + "/cursor_projects";
    std::string cursorChats = baseDir + "/cursor_chats";
    std::string binDir = baseDir + "/bin";
    fs::create_directories(ideBrain);
    fs::create_directories(cliBrain);
    fs::create_directories(claudeProjects);
    fs::create_directories(cursorProjects);
    fs::create_directories(cursorChats);
    fs::create_directories(binDir);

    setenv("AIMON_IDE_BRAIN_DIR", ideBrain.c_str(), 1);
    setenv("AIMON_CLI_BRAIN_DIR", cliBrain.c_str(), 1);
    setenv("AIMON_CLAUDE_PROJECTS_DIR", claudeProjects.c_str(), 1);
    setenv("AIMON_CURSOR_PROJECTS_DIR", cursorProjects.c_str(), 1);
    setenv("AIMON_CURSOR_CHATS_DIR", cursorChats.c_str(), 1);
    setenv("AIMON_WORKSPACE_ROOT", baseDir.c_str(), 1);
    setenv("AIMON_LIVE_ROUTE_TIMEOUT_MS", "3000", 1);
    setenv("AIMON_LIVE_ROUTE_POLL_MS", "30", 1);

    // Create mock screen script in binDir/mock_screen
    std::string mockScreenBin = binDir + "/mock_screen";
    {
        std::ofstream sf(mockScreenBin);
        sf << "#!/bin/bash\n"
           << "LOG_FILE=\"" << baseDir << "/mock_screen.log\"\n"
           << "echo \"$@\" >> \"$LOG_FILE\"\n"
           << "TARGET_FILE=\"" << baseDir << "/target_transcript.txt\"\n"
           << "if [ -f \"$TARGET_FILE\" ]; then\n"
           << "    TRANS_PATH=$(cat \"$TARGET_FILE\")\n"
           << "    LAST_ARG=\"${@: -1}\"\n"
           << "    if [ \"$LAST_ARG\" = $'\r' ] && [ -f \"$TRANS_PATH\" ]; then\n"
           << "        if [[ \"$TRANS_PATH\" == *\"cursor\"* ]]; then\n"
           << "            echo '{\"role\":\"user\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"Live test user prompt\"}]}}' >> \"$TRANS_PATH\"\n"
           << "            echo '{\"role\":\"assistant\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"ACK-SCREEN-CURSOR-OK\"}]}}' >> \"$TRANS_PATH\"\n"
           << "            echo '{\"type\":\"turn_ended\",\"status\":\"success\"}' >> \"$TRANS_PATH\"\n"
           << "        else\n"
           << "            echo '{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"Live test claude prompt\"}}' >> \"$TRANS_PATH\"\n"
           << "            echo '{\"type\":\"assistant\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"ACK-SCREEN-CLAUDE-OK\"}]},\"stop_reason\":\"end_turn\"}' >> \"$TRANS_PATH\"\n"
           << "        fi\n"
           << "    fi\n"
           << "fi\n"
           << "exit 0\n";
        sf.close();
        chmod(mockScreenBin.c_str(), 0755);
    }
    setenv("AIMON_SCREEN_BIN", mockScreenBin.c_str(), 1);
    setenv("AIMON_SCREEN_SESSION_OVERRIDE", "2606765.cursor_agent_test", 1);

    std::string mockAgentBin = binDir + "/agent";
    {
        std::ofstream af(mockAgentBin);
        af << "#!/bin/bash\n"
           << "echo '{\"type\":\"result\",\"result\":\"ACK-CLI-CURSOR-OK\",\"session_id\":\"ba472c97-57a4-4264-9d8f-6065fd61e174\"}'\n"
           << "exit 0\n";
        af.close();
        chmod(mockAgentBin.c_str(), 0755);
    }
    std::string mockClaudeBin = binDir + "/claude";
    {
        std::ofstream cf(mockClaudeBin);
        cf << "#!/bin/bash\n"
           << "echo '{\"type\":\"result\",\"result\":\"ACK-CLI-CLAUDE-OK\",\"session_id\":\"d0001111-2222-3333-4444-555566667777\"}'\n"
           << "exit 0\n";
        cf.close();
        chmod(mockClaudeBin.c_str(), 0755);
    }
    setenv("AIMON_CURSOR_BIN", mockAgentBin.c_str(), 1);
    setenv("AIMON_CLAUDE_BIN", mockClaudeBin.c_str(), 1);

    // 1. Nominal test: Cursor in screen (with store.db)
    std::string cursorCid = "c0001111-2222-3333-4444-555566667777";
    std::string cursorTransDir = cursorProjects + "/test-proj/agent-transcripts/" + cursorCid;
    fs::create_directories(cursorTransDir);
    fs::create_directories(cursorChats + "/test-ws/" + cursorCid);
    {
        std::ofstream sf(cursorChats + "/test-ws/" + cursorCid + "/store.db");
        sf << "sqlite3-store";
    }
    std::string cursorJsonl = cursorTransDir + "/" + cursorCid + ".jsonl";
    {
        std::ofstream jf(cursorJsonl);
        jf << "{\"role\":\"user\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"Prior prompt\"}]}}\n"
           << "{\"role\":\"assistant\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"Prior reply\"}]}}\n"
           << "{\"type\":\"turn_ended\",\"status\":\"success\"}\n";
    }

    {
        std::ofstream tf(baseDir + "/target_transcript.txt");
        tf << cursorJsonl << "\n";
    }

    StateStore& stateStore = StateStore::getInstance();
    HistoryStore historyStore;
    WebConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = 0;
    cfg.endpointsEnabled = false;
    WebServer server(stateStore, historyStore, cfg);
    assert(server.start(true));
    int port = server.getPort();
    assert(port > 0);

    MobileGateway& gw = MobileGateway::getInstance();
    std::string mobileFile = baseDir + "/mobile.json";
    assert(gw.init(mobileFile));
    std::string secret = gw.createPairingSecret(120);
    std::string token;
    assert(gw.verifyPairingCode(secret, "dev-test", "Pixel 9", token));

    httplib::Client client("http://127.0.0.1:" + std::to_string(port));
    httplib::Headers headers = {{"X-Mobile-Token", token}};

    {
        json req = {
            {"agentId", "cursor"},
            {"conversationId", cursorCid},
            {"text", "Hello live cursor"}
        };
        auto res = client.Post("/api/agent/prompt", headers, req.dump(), "application/json");
        assert(res && res->status == 200);
        json rj = json::parse(res->body);
        assert(rj.value("status", "") == "ok");
        assert(rj.value("routedVia", "") == "screen");
        assert(rj.value("screenSession", "") == "2606765.cursor_agent_test");
        assert(rj.value("response", "").find("ACK-SCREEN-CURSOR-OK") != std::string::npos);
    }

    // 2. Nominal test: Claude in screen
    std::string claudeCid = "d0001111-2222-3333-4444-555566667777";
    std::string claudeTransDir = claudeProjects + "/test-proj";
    fs::create_directories(claudeTransDir);
    std::string claudeJsonl = claudeTransDir + "/" + claudeCid + ".jsonl";
    {
        std::ofstream jf(claudeJsonl);
        jf << "{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"Prior claude prompt\"}}\n"
           << "{\"type\":\"assistant\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"Prior claude reply\"}]},\"stop_reason\":\"end_turn\"}\n";
    }

    {
        std::ofstream tf(baseDir + "/target_transcript.txt");
        tf << claudeJsonl << "\n";
    }

    {
        json req = {
            {"agentId", "claude"},
            {"conversationId", claudeCid},
            {"text", "Hello live claude"}
        };
        auto res = client.Post("/api/agent/prompt", headers, req.dump(), "application/json");
        assert(res && res->status == 200);
        json rj = json::parse(res->body);
        assert(rj.value("status", "") == "ok");
        assert(rj.value("routedVia", "") == "screen");
        assert(rj.value("screenSession", "") == "2606765.cursor_agent_test");
        assert(rj.value("response", "").find("ACK-SCREEN-CLAUDE-OK") != std::string::npos);
    }

    // 3. Renderer-owned Cursor IDE session without store.db and without screen is read-only and rejected with 409
    unsetenv("AIMON_SCREEN_SESSION_OVERRIDE");
    std::string ideCid = "ba472c97-57a4-4264-9d8f-6065fd61e174";
    std::string ideTransDir = cursorProjects + "/amba-virt/agent-transcripts/" + ideCid;
    fs::create_directories(ideTransDir);
    std::string ideJsonl = ideTransDir + "/" + ideCid + ".jsonl";
    std::string origIdeContent =
        "{\"role\":\"user\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"Real IDE prompt\"}]}}\n"
        "{\"role\":\"assistant\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"Real IDE reply\"}]}}\n"
        "{\"type\":\"turn_ended\",\"status\":\"success\"}\n";
    {
        std::ofstream jf(ideJsonl);
        jf << origIdeContent;
    }

    {
        auto convRes = client.Get("/api/agent/conversations?provider=cursor", headers);
        assert(convRes && convRes->status == 200);
        json convBody = json::parse(convRes->body);
        bool sawCliInteractive = false;
        bool sawIdeReadOnly = false;
        for (const auto& c : convBody["conversations"]) {
            std::string cidVal = c.value("id", "");
            if (cidVal == cursorCid) {
                sawCliInteractive = true;
                assert(c.value("interactive", false) == true);
                assert(c.value("nonInteractiveReason", "non-empty") == "");
            } else if (cidVal == ideCid) {
                sawIdeReadOnly = true;
                assert(c.value("interactive", true) == false);
                assert(c.value("nonInteractiveReason", "").find(
                    "Cursor IDE GUI session is read-only") != std::string::npos);
            }
        }
        assert(sawCliInteractive);
        assert(sawIdeReadOnly);

        json req = {
            {"agentId", "cursor"},
            {"conversationId", ideCid},
            {"text", "Do not corrupt me"}
        };
        auto res = client.Post("/api/agent/prompt", headers, req.dump(), "application/json");
        assert(res && res->status == 409);
        json rj = json::parse(res->body);
        assert(rj.value("ok", true) == false);
        assert(rj.value("interactive", true) == false);
        assert(rj.value("nonInteractiveReason", "").find(
            "Cursor IDE GUI session is read-only") != std::string::npos);

        std::ifstream checkIn(ideJsonl);
        std::string curContent((std::istreambuf_iterator<char>(checkIn)),
                                std::istreambuf_iterator<char>());
        assert(curContent == origIdeContent);
    }

    // 4. Live non-screen owning process (ownerPid > 0) falls back to CLI --resume without 409 block
    setenv("AIMON_LIVE_OWNER_PID_OVERRIDE", "999999", 1);
    {
        json req = {
            {"agentId", "claude"},
            {"conversationId", claudeCid},
            {"text", "Live process fallback"}
        };
        auto res = client.Post("/api/agent/prompt", headers, req.dump(), "application/json");
        assert(res && res->status == 200);
        json rj = json::parse(res->body);
        assert(rj.value("status", "") == "ok");
        assert(rj.value("response", "").find("ACK-CLI-CLAUDE-OK") != std::string::npos);
    }
    unsetenv("AIMON_LIVE_OWNER_PID_OVERRIDE");

    server.stop();

    unsetenv("AIMON_IDE_BRAIN_DIR");
    unsetenv("AIMON_CLI_BRAIN_DIR");
    unsetenv("AIMON_CLAUDE_PROJECTS_DIR");
    unsetenv("AIMON_CURSOR_PROJECTS_DIR");
    unsetenv("AIMON_CURSOR_CHATS_DIR");
    unsetenv("AIMON_WORKSPACE_ROOT");
    unsetenv("AIMON_LIVE_ROUTE_TIMEOUT_MS");
    unsetenv("AIMON_LIVE_ROUTE_POLL_MS");
    unsetenv("AIMON_SCREEN_BIN");
    unsetenv("AIMON_SCREEN_SESSION_OVERRIDE");
    unsetenv("AIMON_CURSOR_BIN");
    unsetenv("AIMON_CLAUDE_BIN");
    fs::remove_all(baseDir);
    std::cout << "[TestMobileGateway] testLiveScreenSessionRoutingAndIdeSessionProtection passed!" << std::endl;
}

static void Turn1CursorSessionWithSubagentsAndNoParentJsonl_AppearsInConversationsAndTranscript() {
    std::cout << "[TestMobileGateway] Running Turn1CursorSessionWithSubagentsAndNoParentJsonl_AppearsInConversationsAndTranscript..." << std::endl;

    std::string baseDir = "/tmp/test_turn1_cursor_" + std::to_string(getpid());
    fs::remove_all(baseDir);
    std::string ideBrain = baseDir + "/ide_brain";
    std::string cliBrain = baseDir + "/cli_brain";
    std::string claudeProjects = baseDir + "/claude_projects";
    std::string cursorProjects = baseDir + "/cursor_projects";
    std::string cursorChats = baseDir + "/cursor_chats";
    std::string cursorHookState = baseDir + "/cursor_hook_state";
    std::string wsRoot = baseDir + "/work";

    fs::create_directories(ideBrain);
    fs::create_directories(cliBrain);
    fs::create_directories(claudeProjects);
    fs::create_directories(cursorProjects);
    fs::create_directories(cursorChats);
    fs::create_directories(cursorHookState);
    fs::create_directories(wsRoot + "/aimon");

    setenv("AIMON_IDE_BRAIN_DIR", ideBrain.c_str(), 1);
    setenv("AIMON_CLI_BRAIN_DIR", cliBrain.c_str(), 1);
    setenv("AIMON_CLAUDE_PROJECTS_DIR", claudeProjects.c_str(), 1);
    setenv("AIMON_CURSOR_PROJECTS_DIR", cursorProjects.c_str(), 1);
    setenv("AIMON_CURSOR_CHATS_DIR", cursorChats.c_str(), 1);
    setenv("AIMON_CURSOR_HOOK_STATE_DIR", cursorHookState.c_str(), 1);
    setenv("AIMON_WORKSPACE_ROOT", wsRoot.c_str(), 1);

    // Parent Turn-1 Cursor session: directory exists with subagents/*.jsonl, NO parent <cid>.jsonl
    std::string turn1Cid = "c0ffee11-2222-3333-4444-555566667777";
    std::string subDir = cursorProjects + "/home-samurai-work-aimon/agent-transcripts/" + turn1Cid + "/subagents";
    fs::create_directories(subDir);

    std::string subAId = "subagent-a111-2222-3333-444455556666";
    {
        std::ofstream sf(subDir + "/" + subAId + ".jsonl");
        sf << "{\"role\":\"user\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"Subagent A task\"}]}}\n"
           << "{\"role\":\"assistant\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"Subagent A findings\"}]}}\n";
    }
    std::string subBId = "subagent-b111-2222-3333-444455556666";
    {
        std::ofstream sf(subDir + "/" + subBId + ".jsonl");
        sf << "{\"role\":\"user\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"Subagent B task\"}]}}\n"
           << "{\"role\":\"assistant\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"Subagent B findings\"}]}}\n";
    }

    // Hook state for Turn-1 parent session (note: even if is_subagent was polluted true by subagentStart,
    // presence of subagents/ and RUNNING status must keep it as a main session)
    int64_t nowEpoch = static_cast<int64_t>(std::time(nullptr));
    {
        std::ofstream hf(cursorHookState + "/" + turn1Cid + ".json");
        json hj = {
            {"conversation_id", turn1Cid},
            {"status", "RUNNING"},
            {"is_subagent", true},
            {"model", "claude-4.6-opus-max-thinking"},
            {"workspace_path", wsRoot + "/aimon"},
            {"first_prompt", "Turn 1 live prompt"},
            {"last_prompt", "Turn 1 live prompt"},
            {"updated_epoch", nowEpoch}
        };
        hf << hj.dump(2);
    }
    {
        std::ofstream hjf(cursorHookState + "/" + turn1Cid + ".jsonl");
        hjf << "{\"role\":\"user\",\"timestamp\":" << nowEpoch
            << ",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"Turn 1 live prompt\"}]}}\n";
    }

    // Record in AgentTelemetryDb as well
    auto& db = AgentTelemetryDb::getInstance();
    db.recordSessionStart(
        turn1Cid, turn1Cid, "cursor",
        wsRoot + "/aimon", "claude-4.6-opus-max-thinking",
        nowEpoch
    );

    // Also create a real standalone subagent session and orphan hook state files that MUST remain excluded
    std::string realSubCid = "deadbeef-0000-1111-2222-333344445555";
    {
        std::ofstream hf(cursorHookState + "/" + realSubCid + ".json");
        json hj = {
            {"conversation_id", realSubCid},
            {"status", "DONE"},
            {"is_subagent", true},
            {"updated_epoch", nowEpoch}
        };
        hf << hj.dump(2);
    }
    std::string orphanHookCid = "0c9cb9a0-0be5-43c2-8c99-60ad38f6dbfe";
    {
        std::ofstream hf(cursorHookState + "/" + orphanHookCid + ".json");
        json hj = {
            {"session_id", orphanHookCid},
            {"workspace", wsRoot + "/aimon"},
            {"model", "unknown"},
            {"start_ts", nowEpoch},
            {"step", 6},
            {"tools", 1},
            {"turns", 5}
        };
        hf << hj.dump(2);
    }
    {
        std::ofstream hf(cursorHookState + "/sess-probeagy.json");
        json hj = {
            {"session_id", "sess-probeagy"},
            {"workspace", wsRoot + "/aimon"},
            {"status", "RUNNING"},
            {"start_ts", nowEpoch}
        };
        hf << hj.dump(2);
    }

    StateStore& stateStore = StateStore::getInstance();
    HistoryStore historyStore;
    WebConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = 0;
    cfg.endpointsEnabled = false;
    WebServer server(stateStore, historyStore, cfg);
    assert(server.start(true));
    int port = server.getPort();

    MobileGateway& gw = MobileGateway::getInstance();
    std::string mobileFile = baseDir + "/mobile.json";
    assert(gw.init(mobileFile));
    std::string secret = gw.createPairingSecret(120);
    std::string token;
    assert(gw.verifyPairingCode(secret, "dev-turn1", "Pixel 9", token));

    httplib::Client client("http://127.0.0.1:" + std::to_string(port));
    httplib::Headers headers = {{"X-Mobile-Token", token}};

    // 1. Verify GET /api/agent/conversations?agent=cursor includes turn1Cid and excludes realSubCid
    {
        auto res = client.Get("/api/agent/conversations?agent=cursor", headers);
        assert(res && res->status == 200);
        json rj = json::parse(res->body);
        assert(rj.contains("conversations") && rj["conversations"].is_array());
        bool foundTurn1 = false;
        bool foundRealSub = false;
        for (const auto& c : rj["conversations"]) {
            std::string cid = c.value("id", "");
            if (cid == turn1Cid) {
                foundTurn1 = true;
                assert(c.value("active", false) == true);
                assert(c.value("status", "") == "RUNNING");
                assert(c.value("stepCount", 0) >= 3);
                assert(c.value("firstPrompt", "") == "Turn 1 live prompt");
            }
            if (cid == realSubCid || cid == subAId || cid == subBId ||
                cid == orphanHookCid || cid == "sess-probeagy") {
                foundRealSub = true;
            }
            assert(c.value("title", "").find("Active Cursor IDE session in ") == std::string::npos);
        }
        assert(foundTurn1);
        assert(!foundRealSub);
    }

    // 2. Verify GET /api/agent/transcript?id=<turn1Cid>&agent=cursor returns synthesized parent + subagent steps
    {
        auto res = client.Get(("/api/agent/transcript?id=" + turn1Cid + "&agent=cursor").c_str(), headers);
        assert(res && res->status == 200);
        json rj = json::parse(res->body);
        assert(rj.value("conversationId", "") == turn1Cid);
        assert(rj.value("status", "") == "RUNNING");
        assert(rj.value("stepCount", 0) >= 3);
        const auto& steps = rj["steps"];
        assert(steps.is_array() && steps.size() >= 3);
        assert(steps[0].value("type", "") == "USER_INPUT");
        assert(steps[0].value("text", "") == "Turn 1 live prompt");
        bool sawSubagentStep = false;
        for (size_t i = 1; i < steps.size(); ++i) {
            if (steps[i].value("isSubagentStep", false)) {
                sawSubagentStep = true;
                assert(!steps[i].value("subagentId", "").empty());
            }
        }
        assert(sawSubagentStep);
    }

    // 3. Verify WebServer::diagnoseSessions and WebServer::inspectSession
    {
        json diagList = WebServer::diagnoseSessions("cursor");
        assert(diagList.value("ok", false) == true);
        assert(diagList["sessions"].is_array() && !diagList["sessions"].empty());

        json inspected = WebServer::inspectSession(turn1Cid);
        assert(inspected.value("ok", false) == true);
        assert(inspected.value("provider", "") == "cursor");
        assert(inspected["transcript"].value("step_count", 0) >= 3);
        assert(inspected["transcript"].value("subagent_file_count", 0) == 2);
    }

    server.stop();
    unsetenv("AIMON_IDE_BRAIN_DIR");
    unsetenv("AIMON_CLI_BRAIN_DIR");
    unsetenv("AIMON_CLAUDE_PROJECTS_DIR");
    unsetenv("AIMON_CURSOR_PROJECTS_DIR");
    unsetenv("AIMON_CURSOR_CHATS_DIR");
    unsetenv("AIMON_CURSOR_HOOK_STATE_DIR");
    unsetenv("AIMON_WORKSPACE_ROOT");
    fs::remove_all(baseDir);
    std::cout << "[TestMobileGateway] Turn1CursorSessionWithSubagentsAndNoParentJsonl_AppearsInConversationsAndTranscript passed!" << std::endl;
}

static void NonInteractiveCursorAndClaudeIdeSessions_ReadOnlyInConversationsAndRejectedOnPromptWith409() {
    std::cout << "[TestMobileGateway] Running NonInteractiveCursorAndClaudeIdeSessions_ReadOnlyInConversationsAndRejectedOnPromptWith409..." << std::endl;

    std::string baseDir = "/tmp/test_readonly_ide_" + std::to_string(getpid());
    fs::remove_all(baseDir);
    std::string ideBrain = baseDir + "/ide_brain";
    std::string cliBrain = baseDir + "/cli_brain";
    std::string claudeProjects = baseDir + "/claude_projects";
    std::string cursorProjects = baseDir + "/cursor_projects";
    std::string cursorChats = baseDir + "/cursor_chats";
    std::string cursorHookState = baseDir + "/cursor_hook_state";
    std::string wsRoot = baseDir + "/work";
    std::string binDir = baseDir + "/bin";

    fs::create_directories(ideBrain);
    fs::create_directories(cliBrain);
    fs::create_directories(claudeProjects + "/-home-samurai-work-aimon");
    fs::create_directories(cursorProjects);
    fs::create_directories(cursorChats);
    fs::create_directories(cursorHookState);
    fs::create_directories(wsRoot + "/aimon");
    fs::create_directories(binDir);

    setenv("AIMON_IDE_BRAIN_DIR", ideBrain.c_str(), 1);
    setenv("AIMON_CLI_BRAIN_DIR", cliBrain.c_str(), 1);
    setenv("AIMON_CLAUDE_PROJECTS_DIR", claudeProjects.c_str(), 1);
    setenv("AIMON_CURSOR_PROJECTS_DIR", cursorProjects.c_str(), 1);
    setenv("AIMON_CURSOR_CHATS_DIR", cursorChats.c_str(), 1);
    setenv("AIMON_CURSOR_HOOK_STATE_DIR", cursorHookState.c_str(), 1);
    setenv("AIMON_WORKSPACE_ROOT", wsRoot.c_str(), 1);

    // 1. Cursor IDE GUI session (NO store.db / prompt_history.json, NO screen session)
    std::string cursorIdeCid = "cursor-ide-gui-session-9999";
    std::string cursorIdeDir = cursorProjects + "/home-samurai-work-aimon/agent-transcripts/" + cursorIdeCid;
    fs::create_directories(cursorIdeDir);
    std::string cursorIdePath = cursorIdeDir + "/" + cursorIdeCid + ".jsonl";
    {
        std::ofstream tf(cursorIdePath);
        tf << "{\"role\":\"user\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"First historic user question\"}]}}\n"
           << "{\"role\":\"assistant\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"First historic assistant answer\"}]}}\n";
    }

    // 2. Cursor CLI session (WITH store.db)
    std::string cursorCliCid = "cursor-cli-session-6666";
    std::string cursorCliDir = cursorProjects + "/home-samurai-work-aimon/agent-transcripts/" + cursorCliCid;
    fs::create_directories(cursorCliDir);
    fs::create_directories(cursorChats + "/ws-hash/" + cursorCliCid);
    {
        std::ofstream tf(cursorCliDir + "/" + cursorCliCid + ".jsonl");
        tf << "{\"role\":\"user\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"Cursor CLI user question\"}]}}\n"
           << "{\"role\":\"assistant\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"Cursor CLI assistant answer\"}]}}\n";
        std::ofstream sf(cursorChats + "/ws-hash/" + cursorCliCid + "/store.db");
        sf << "sqlite3-store";
    }

    // 3. Claude Code IDE Extension session (with <ide_opened_file> and entrypoint: claude-vscode, NO screen session)
    std::string claudeIdeCid = "claude-ide-ext-session-8888";
    std::string claudeIdePath = claudeProjects + "/-home-samurai-work-aimon/" + claudeIdeCid + ".jsonl";
    {
        std::ofstream cf(claudeIdePath);
        cf << "{\"type\":\"user\",\"entrypoint\":\"claude-vscode\",\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"<ide_opened_file>/tmp/foo.cxx</ide_opened_file>\"},{\"type\":\"text\",\"text\":\"Claude IDE extension prompt\"}]}}\n"
           << "{\"type\":\"assistant\",\"entrypoint\":\"claude-vscode\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"Claude IDE extension reply\"}]}}\n";
    }

    // 4. Interactive Claude CLI session (entrypoint: cli)
    std::string claudeCliCid = "claude-cli-session-7777";
    std::string claudeCliPath = claudeProjects + "/-home-samurai-work-aimon/" + claudeCliCid + ".jsonl";
    {
        std::ofstream cf(claudeCliPath);
        cf << "{\"type\":\"user\",\"entrypoint\":\"cli\",\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"Claude CLI prompt\"}]}}\n"
           << "{\"type\":\"assistant\",\"entrypoint\":\"cli\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"Claude CLI reply\"}]}}\n";
    }

    // 5. Boundary: Claude session that started in claude-vscode and later transitioned to cli
    std::string claudeTransCid = "claude-transitioned-session-5555";
    std::string claudeTransPath = claudeProjects + "/-home-samurai-work-aimon/" + claudeTransCid + ".jsonl";
    {
        std::ofstream cf(claudeTransPath);
        cf << "{\"type\":\"user\",\"entrypoint\":\"claude-vscode\",\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"Initial VSCode turn\"}]}}\n"
           << "{\"type\":\"assistant\",\"entrypoint\":\"claude-vscode\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"Initial VSCode reply\"}]}}\n"
           << "{\"type\":\"user\",\"entrypoint\":\"cli\",\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"Resumed in CLI turn\"}]}}\n"
           << "{\"type\":\"assistant\",\"entrypoint\":\"cli\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"Resumed in CLI reply\"}]}}\n";
    }

    std::string capturedCursorArgs = baseDir + "/captured_cursor_args.txt";
    std::string mockCursorBin = binDir + "/agent";
    {
        std::ofstream mf(mockCursorBin);
        mf << "#!/bin/bash\n"
           << "printf '%s\\n' \"$@\" > \"" << capturedCursorArgs << "\"\n"
           << "echo '{\"type\":\"result\",\"result\":\"Continued reply from Cursor CLI\",\"session_id\":\"" << cursorCliCid << "\"}'\n"
           << "exit 0\n";
        mf.close();
        chmod(mockCursorBin.c_str(), 0755);
    }
    setenv("AIMON_CURSOR_BIN", mockCursorBin.c_str(), 1);

    std::string capturedClaudeArgs = baseDir + "/captured_claude_args.txt";
    std::string mockClaudeBin = binDir + "/claude";
    {
        std::ofstream mf(mockClaudeBin);
        mf << "#!/bin/bash\n"
           << "printf '%s\\n' \"$@\" > \"" << capturedClaudeArgs << "\"\n"
           << "echo '{\"type\":\"result\",\"result\":\"Continued reply from Claude CLI\",\"session_id\":\"" << claudeCliCid << "\"}'\n"
           << "exit 0\n";
        mf.close();
        chmod(mockClaudeBin.c_str(), 0755);
    }
    setenv("AIMON_CLAUDE_BIN", mockClaudeBin.c_str(), 1);

    StateStore& stateStore = StateStore::getInstance();
    HistoryStore historyStore;
    WebConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = 0;
    cfg.endpointsEnabled = false;
    WebServer server(stateStore, historyStore, cfg);
    assert(server.start(true));
    int port = server.getPort();

    MobileGateway& gw = MobileGateway::getInstance();
    std::string mobileFile = baseDir + "/mobile.json";
    assert(gw.init(mobileFile));
    std::string secret = gw.createPairingSecret(120);
    std::string token;
    assert(gw.verifyPairingCode(secret, "dev-readonly", "Pixel 9", token));

    httplib::Client client("http://127.0.0.1:" + std::to_string(port));
    httplib::Headers headers = {{"X-Mobile-Token", token}};

    // A. Assert via GET /api/agent/conversations?provider=cursor
    {
        auto res = client.Get("/api/agent/conversations?provider=cursor", headers);
        assert(res && res->status == 200);
        json rj = json::parse(res->body);
        bool foundCursorIde = false;
        bool foundCursorCli = false;
        for (const auto& c : rj["conversations"]) {
            std::string cid = c.value("id", "");
            if (cid == cursorIdeCid) {
                foundCursorIde = true;
                assert(c.value("interactive", true) == false);
                assert(c.value("nonInteractiveReason", "").find(
                    "Cursor IDE GUI session is read-only") != std::string::npos);
            } else if (cid == cursorCliCid) {
                foundCursorCli = true;
                assert(c.value("interactive", false) == true);
                assert(c.value("nonInteractiveReason", "non-empty") == "");
            }
        }
        assert(foundCursorIde);
        assert(foundCursorCli);
    }

    // B. Assert via GET /api/agent/transcript?provider=cursor&conversationId=cursor-ide-gui-session-9999
    {
        auto tres = client.Get(("/api/agent/transcript?provider=cursor&conversationId=" + cursorIdeCid).c_str(), headers);
        assert(tres && tres->status == 200);
        json tj = json::parse(tres->body);
        assert(tj.value("stepCount", 0) >= 2);
        assert(tj.value("interactive", true) == false);
        assert(tj.value("nonInteractiveReason", "").find(
            "Cursor IDE GUI session is read-only") != std::string::npos);
    }

    // C. Assert via POST /api/agent/prompt for cursor-ide-gui-session-9999 -> HTTP 409 Conflict
    {
        json req = {
            {"provider", "cursor"},
            {"conversationId", cursorIdeCid},
            {"prompt", "Follow up from mobile"}
        };
        auto res = client.Post("/api/agent/prompt", headers, req.dump(), "application/json");
        assert(res && res->status == 409);
        json rj = json::parse(res->body);
        assert(rj.value("ok", true) == false);
        assert(rj.value("interactive", true) == false);
        assert(rj.value("nonInteractiveReason", "").find(
            "Cursor IDE GUI session is read-only") != std::string::npos);
        assert(!fs::exists(capturedCursorArgs));

        auto dres = client.Get(("/api/agent/diagnostics?conversationId=" + cursorIdeCid).c_str(), headers);
        assert(dres && dres->status == 200);
        json dj = json::parse(dres->body);
        assert(dj.value("interactive", true) == false);
        assert(dj.value("selected_route", "") == "rejected_non_interactive_ide");
        assert(dj.value("recommended_route", "") == "rejected_non_interactive_ide");
    }

    // D. Assert via GET /api/agent/conversations?provider=claude
    {
        auto res = client.Get("/api/agent/conversations?provider=claude", headers);
        assert(res && res->status == 200);
        json rj = json::parse(res->body);
        bool foundClaudeIde = false;
        bool foundClaudeCli = false;
        bool foundClaudeTrans = false;
        for (const auto& c : rj["conversations"]) {
            std::string cid = c.value("id", "");
            if (cid == claudeIdeCid) {
                foundClaudeIde = true;
                assert(c.value("interactive", true) == false);
                assert(c.value("nonInteractiveReason", "").find(
                    "Claude Code IDE extension session is read-only") != std::string::npos);
            } else if (cid == claudeCliCid) {
                foundClaudeCli = true;
                assert(c.value("interactive", false) == true);
                assert(c.value("nonInteractiveReason", "non-empty") == "");
            } else if (cid == claudeTransCid) {
                foundClaudeTrans = true;
                assert(c.value("interactive", false) == true);
                assert(c.value("nonInteractiveReason", "non-empty") == "");
            }
        }
        assert(foundClaudeIde);
        assert(foundClaudeCli);
        assert(foundClaudeTrans);
    }

    // E. Assert via GET /api/agent/transcript?provider=claude&conversationId=claude-ide-ext-session-8888
    {
        auto tres = client.Get(("/api/agent/transcript?provider=claude&conversationId=" + claudeIdeCid).c_str(), headers);
        assert(tres && tres->status == 200);
        json tj = json::parse(tres->body);
        assert(tj.value("stepCount", 0) >= 2);
        assert(tj.value("interactive", true) == false);
        assert(tj.value("nonInteractiveReason", "").find(
            "Claude Code IDE extension session is read-only") != std::string::npos);
    }

    // F. Assert via POST /api/agent/prompt for claude-ide-ext-session-8888 -> HTTP 409 Conflict
    {
        json req = {
            {"provider", "claude"},
            {"conversationId", claudeIdeCid},
            {"prompt", "Follow up to Claude IDE"}
        };
        auto res = client.Post("/api/agent/prompt", headers, req.dump(), "application/json");
        assert(res && res->status == 409);
        json rj = json::parse(res->body);
        assert(rj.value("ok", true) == false);
        assert(rj.value("interactive", true) == false);
        assert(rj.value("nonInteractiveReason", "").find(
            "Claude Code IDE extension session is read-only") != std::string::npos);
        assert(!fs::exists(capturedClaudeArgs));

        auto dres = client.Get(("/api/agent/diagnostics?conversationId=" + claudeIdeCid).c_str(), headers);
        assert(dres && dres->status == 200);
        json dj = json::parse(dres->body);
        assert(dj.value("interactive", true) == false);
        assert(dj.value("selected_route", "") == "rejected_non_interactive_ide");
        assert(dj.value("recommended_route", "") == "rejected_non_interactive_ide");
    }

    // G. Assert local slash commands (/status, /help) still succeed with 200 OK on read-only sessions,
    // and interactive CLI sessions (cursorCliCid, claudeCliCid) succeed with 200 OK on POST /api/agent/prompt
    {
        json slashReq = {
            {"provider", "cursor"},
            {"conversationId", cursorIdeCid},
            {"prompt", "/status"}
        };
        auto sres = client.Post("/api/agent/prompt", headers, slashReq.dump(), "application/json");
        assert(sres && sres->status == 200);

        json cliCursorReq = {
            {"provider", "cursor"},
            {"conversationId", cursorCliCid},
            {"prompt", "Continue Cursor CLI"}
        };
        auto cres = client.Post("/api/agent/prompt", headers, cliCursorReq.dump(), "application/json");
        assert(cres && cres->status == 200);

        json cliClaudeReq = {
            {"provider", "claude"},
            {"conversationId", claudeCliCid},
            {"prompt", "Continue Claude CLI"}
        };
        auto clres = client.Post("/api/agent/prompt", headers, cliClaudeReq.dump(), "application/json");
        assert(clres && clres->status == 200);
    }

    server.stop();
    unsetenv("AIMON_IDE_BRAIN_DIR");
    unsetenv("AIMON_CLI_BRAIN_DIR");
    unsetenv("AIMON_CLAUDE_PROJECTS_DIR");
    unsetenv("AIMON_CURSOR_PROJECTS_DIR");
    unsetenv("AIMON_CURSOR_CHATS_DIR");
    unsetenv("AIMON_CURSOR_HOOK_STATE_DIR");
    unsetenv("AIMON_WORKSPACE_ROOT");
    unsetenv("AIMON_CURSOR_BIN");
    unsetenv("AIMON_CLAUDE_BIN");
    fs::remove_all(baseDir);
    std::cout << "[TestMobileGateway] NonInteractiveCursorAndClaudeIdeSessions_ReadOnlyInConversationsAndRejectedOnPromptWith409 passed!" << std::endl;
}

static void FourHopDiagnosticTrace_CorrelatesRequestIdAndScrubsTokens() {
    std::cout << "[TestMobileGateway] Running FourHopDiagnosticTrace_CorrelatesRequestIdAndScrubsTokens..." << std::endl;

    {
        DiagnosticsConfig d;
        d.enabled = true;
        d.logMobileHttp = true;
        d.logRouterDecisions = true;
        d.logIdleTranscriptPolls = true;
        d.logDiscoveryPolls = true;
        d.logHooks = true;
        AgentTelemetryDb::getInstance().setDiagnosticsConfig(d);
    }

    std::string baseDir = "/tmp/test_4hop_trace_" + std::to_string(getpid());
    fs::remove_all(baseDir);
    std::string ideBrain = baseDir + "/ide_brain";
    std::string cliBrain = baseDir + "/cli_brain";
    std::string claudeProjects = baseDir + "/claude_projects";
    std::string cursorProjects = baseDir + "/cursor_projects";
    std::string cursorChats = baseDir + "/cursor_chats";
    std::string cursorHookState = baseDir + "/cursor_hook_state";
    std::string wsRoot = baseDir + "/work";
    std::string binDir = baseDir + "/bin";

    fs::create_directories(ideBrain);
    fs::create_directories(cliBrain);
    fs::create_directories(claudeProjects);
    fs::create_directories(cursorProjects);
    fs::create_directories(cursorChats);
    fs::create_directories(cursorHookState);
    fs::create_directories(wsRoot + "/aimon");
    fs::create_directories(binDir);

    setenv("AIMON_IDE_BRAIN_DIR", ideBrain.c_str(), 1);
    setenv("AIMON_CLI_BRAIN_DIR", cliBrain.c_str(), 1);
    setenv("AIMON_CLAUDE_PROJECTS_DIR", claudeProjects.c_str(), 1);
    setenv("AIMON_CURSOR_PROJECTS_DIR", cursorProjects.c_str(), 1);
    setenv("AIMON_CURSOR_CHATS_DIR", cursorChats.c_str(), 1);
    setenv("AIMON_CURSOR_HOOK_STATE_DIR", cursorHookState.c_str(), 1);
    setenv("AIMON_WORKSPACE_ROOT", wsRoot.c_str(), 1);

    std::string traceCid = "f00dbabe-1111-2222-3333-444455556666";
    std::string transDir = cursorProjects + "/home-samurai-work-aimon/agent-transcripts/" + traceCid;
    fs::create_directories(transDir);
    fs::create_directories(cursorChats + "/ws-hash/" + traceCid);
    {
        std::ofstream tf(transDir + "/" + traceCid + ".jsonl");
        tf << "{\"role\":\"user\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"Initial trace prompt\"}]}}\n"
           << "{\"role\":\"assistant\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"Initial trace reply\"}]}}\n";
        std::ofstream sf(cursorChats + "/ws-hash/" + traceCid + "/store.db");
        sf << "sqlite3-store";
    }

    std::string mockCursorBin = binDir + "/agent";
    {
        std::ofstream mf(mockCursorBin);
        mf << "#!/bin/bash\n"
           << "echo '{\"type\":\"result\",\"result\":\"Trace reply OK\",\"session_id\":\"" << traceCid << "\"}'\n"
           << "exit 0\n";
        mf.close();
        chmod(mockCursorBin.c_str(), 0755);
    }
    setenv("AIMON_CURSOR_BIN", mockCursorBin.c_str(), 1);

    StateStore& stateStore = StateStore::getInstance();
    HistoryStore historyStore;
    WebConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = 0;
    cfg.endpointsEnabled = false;
    WebServer server(stateStore, historyStore, cfg);
    assert(server.start(true));
    int port = server.getPort();

    MobileGateway& gw = MobileGateway::getInstance();
    std::string mobileFile = baseDir + "/mobile.json";
    assert(gw.init(mobileFile));
    std::string secret = gw.createPairingSecret(120);
    std::string token;
    assert(gw.verifyPairingCode(secret, "dev-trace", "Pixel 9", token));
    assert(!token.empty());

    httplib::Client client("http://127.0.0.1:" + std::to_string(port));

    // Verify User-Agent: aimon-hook without valid auth token is rejected with 401
    {
        httplib::Headers spoofHeaders = {{"User-Agent", "aimon-hook"}};
        auto unauthRes = client.Get("/api/agent/conversations?agent=cursor", spoofHeaders);
        assert(unauthRes && unauthRes->status == 401);
    }

    std::string reqId = "req-trace-4hop-01";
    httplib::Headers headers = {
        {"X-Mobile-Token", token},
        {"Authorization", "Bearer " + token},
        {"X-Request-Id", reqId},
        {"X-Client-Meta", "android"}
    };

    auto r1 = client.Get("/api/agent/conversations?agent=cursor", headers);
    assert(r1 && r1->status == 200);

    auto r2 = client.Get(("/api/agent/transcript?id=" + traceCid + "&agent=cursor").c_str(), headers);
    assert(r2 && r2->status == 200);

    json promptReq = {
        {"agentId", "cursor"},
        {"conversationId", traceCid},
        {"text", "Trace hop prompt"}
    };
    auto r3 = client.Post("/api/agent/prompt", headers, promptReq.dump(), "application/json");
    assert(r3 && r3->status == 200);

    json traceResult = AgentTelemetryDb::getInstance().queryTrace(reqId, 500);
    std::string rawTraceDump = traceResult.dump();

    // Token must never appear in raw form in diagnostic logs
    assert(rawTraceDump.find(token) == std::string::npos);

    const auto& logs = traceResult["trace_events"];
    assert(logs.is_array() && !logs.empty());

    bool sawMobileHttp = false;
    bool sawDiscovery = false;
    bool sawTranscript = false;
    bool sawRouterDecision = false;
    bool sawPlatformForward = false;

    for (const auto& entry : logs) {
        assert(entry.value("request_id", "") == reqId);
        std::string cat = entry.value("category", "");
        if (cat == "mobile_http") {
            sawMobileHttp = true;
            if (entry.contains("details") && entry["details"].is_object()) {
                std::string fp = entry["details"].value("token_fp", "");
                assert(fp.size() == 12);
                assert(fp != token);
            }
        } else if (cat == "discovery") {
            sawDiscovery = true;
        } else if (cat == "transcript") {
            sawTranscript = true;
        } else if (cat == "router_decision") {
            sawRouterDecision = true;
        } else if (cat == "platform_forward") {
            sawPlatformForward = true;
        }
    }

    assert(sawMobileHttp);
    assert(sawDiscovery);
    assert(sawTranscript);
    assert(sawRouterDecision);
    assert(sawPlatformForward);

    server.stop();
    unsetenv("AIMON_IDE_BRAIN_DIR");
    unsetenv("AIMON_CLI_BRAIN_DIR");
    unsetenv("AIMON_CLAUDE_PROJECTS_DIR");
    unsetenv("AIMON_CURSOR_PROJECTS_DIR");
    unsetenv("AIMON_CURSOR_CHATS_DIR");
    unsetenv("AIMON_CURSOR_HOOK_STATE_DIR");
    unsetenv("AIMON_WORKSPACE_ROOT");
    unsetenv("AIMON_CURSOR_BIN");
    fs::remove_all(baseDir);
    std::cout << "[TestMobileGateway] FourHopDiagnosticTrace_CorrelatesRequestIdAndScrubsTokens passed!" << std::endl;
}

static void RuntimeConfigAndDiagnosticsControlPlane_DynamicLockedCliFallbackAndPurge() {
    std::cout << "[TestMobileGateway] Running RuntimeConfigAndDiagnosticsControlPlane_DynamicLockedCliFallbackAndPurge..." << std::endl;

    // Reset singleton DiagnosticsConfig to defaults (all switches OFF by default)
    AgentTelemetryDb::getInstance().setDiagnosticsConfig(DiagnosticsConfig{});
    AgentTelemetryDb::getInstance().clearDiagnostics();

    std::string baseDir = "/tmp/test_runtime_cfg_diag_" + std::to_string(getpid());
    fs::remove_all(baseDir);
    std::string ideBrain = baseDir + "/ide_brain";
    std::string cliBrain = baseDir + "/cli_brain";
    std::string presenceDir = baseDir + "/presence";
    std::string procsFile = baseDir + "/running_agy.json";
    std::string epFile = baseDir + "/agy_ls_endpoints.json";
    std::string wsRoot = baseDir + "/work/aimon";
    std::string binDir = baseDir + "/bin";

    fs::create_directories(ideBrain);
    fs::create_directories(cliBrain);
    fs::create_directories(presenceDir);
    fs::create_directories(wsRoot);
    fs::create_directories(binDir);

    std::string lockedCid = "410f5eb3-08b6-40c1-821a-b5ad5857d59b";
    fs::path convDir = fs::path(cliBrain) / lockedCid / ".system_generated" / "logs";
    fs::create_directories(convDir);
    {
        std::ofstream tf(convDir / "transcript.jsonl");
        tf << "{\"step_index\":0,\"type\":\"USER_INPUT\",\"status\":\"DONE\",\"content\":\"<USER_REQUEST>Initial CLI prompt</USER_REQUEST>\"}\n"
           << "{\"step_index\":1,\"type\":\"PLANNER_RESPONSE\",\"status\":\"DONE\",\"content\":\"Initial CLI response\",\"tool_calls\":[]}\n";
    }

    // Create a live lock file for lockedCid and a stale lock file for a dead session
    {
        std::ofstream lf1(presenceDir + "/" + lockedCid + ".lock");
        lf1 << "999888\n";
        std::ofstream lf2(presenceDir + "/stale-dead-session.lock");
        lf2 << "111222\n";
    }

    // Running agy process locking lockedCid on port 45231 with NO CSRF token in epFile
    {
        std::ofstream pf(procsFile);
        json pj = {
            {"processes", json::array({
                {
                    {"pid", 999888},
                    {"cwd", wsRoot},
                    {"lockedConversationId", lockedCid},
                    {"terminalPts", "pts/19"},
                    {"httpPort", 45231},
                    {"httpsPort", 45232}
                }
            })}
        };
        pf << pj.dump(2);
    }
    {
        std::ofstream ef(epFile);
        ef << "{\"endpoints\":{}}\n";
    }

    std::string agyArgsFile = baseDir + "/agy_resume_args.txt";
    std::string mockAgyBin = binDir + "/agy";
    {
        std::ofstream mf(mockAgyBin);
        mf << "#!/bin/bash\n"
           << "printf '%s\\n' \"$@\" > \"" << agyArgsFile << "\"\n"
           << "echo \"Resumed locked CLI session ok\"\n"
           << "exit 0\n";
        mf.close();
        chmod(mockAgyBin.c_str(), 0755);
    }

    setenv("AIMON_IDE_BRAIN_DIR", ideBrain.c_str(), 1);
    setenv("AIMON_CLI_BRAIN_DIR", cliBrain.c_str(), 1);
    setenv("AIMON_CLI_PRESENCE_DIR", presenceDir.c_str(), 1);
    setenv("AIMON_AGY_PROCESSES_FILE", procsFile.c_str(), 1);
    setenv("AIMON_AGY_ENDPOINTS_FILE", epFile.c_str(), 1);
    setenv("AIMON_WORKSPACE_ROOT", wsRoot.c_str(), 1);
    setenv("AIMON_AGY_BIN", mockAgyBin.c_str(), 1);
    unsetenv("AGY_LS_PORT");
    unsetenv("AGY_LS_CSRF_TOKEN");

    StateStore& stateStore = StateStore::getInstance();
    HistoryStore historyStore;
    WebConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = 0;
    cfg.endpointsEnabled = false;
    WebServer server(stateStore, historyStore, cfg);
    assert(server.start(true));
    int port = server.getPort();

    MobileGateway& gw = MobileGateway::getInstance();
    std::string mobileFile = baseDir + "/mobile.json";
    assert(gw.init(mobileFile));
    std::string secret = gw.createPairingSecret(120);
    std::string token;
    assert(gw.verifyPairingCode(secret, "dev-cfg-plane", "Pixel 9 Pro", token));

    httplib::Client client("http://127.0.0.1:" + std::to_string(port));
    httplib::Headers headers = {{"X-Mobile-Token", token}};

    // 1. Verify GET /api/config returns defaults (all diagnostics switches OFF by default, Fallback 5 ON)
    {
        auto rCfg = client.Get("/api/config", headers);
        assert(rCfg && rCfg->status == 200);
        json cj = json::parse(rCfg->body);
        assert(cj.value("ok", false) == true);
        assert(cj.value("stale_presence_locks_count", 0) == 1);
        const auto& gwJson = cj["config"]["gateway"];
        const auto& dgJson = cj["config"]["diagnostics"];
        assert(gwJson.value("allow_cli_locked_detached_resume", false) == true);
        assert(gwJson.value("allow_cli_new_session_spawn", false) == true);
        assert(gwJson.value("allow_agy_agentapi_fallback", false) == true);
        assert(dgJson.value("enabled", true) == false);
        assert(dgJson.value("log_mobile_http", true) == false);
        assert(dgJson.value("log_router_decisions", true) == false);
        assert(dgJson.value("log_idle_transcript_polls", true) == false);
        assert(dgJson.value("log_discovery_polls", true) == false);
        assert(dgJson.value("log_hooks", true) == false);
    }

    // 2. Verify GET /api/agent/diagnostics?id=<lockedCid> and GET /api/agent/diagnostics inspect the session and recommend agy_cli_resume
    {
        auto rDiag = client.Get(("/api/agent/diagnostics?id=" + lockedCid).c_str(), headers);
        assert(rDiag && rDiag->status == 200);
        json dj = json::parse(rDiag->body);
        assert(dj.value("ok", false) == true);
        assert(dj.value("conversation_id", "") == lockedCid);
        assert(dj.value("provider", "") == "antigravity");
        assert(dj.value("source", "") == "cli");
        assert(dj.value("open_in_terminal", false) == true);
        assert(dj.value("locked_by_running_agy", false) == true);
        assert(dj.value("http_port", 0) == 45231);
        assert(dj.value("has_registered_csrf", true) == false);
        assert(dj.value("recommended_route", "") == "agy_cli_resume");
        assert(dj.value("interactive", false) == true);

        auto rDiagAll = client.Get("/api/agent/diagnostics", headers);
        assert(rDiagAll && rDiagAll->status == 200);
        json djAll = json::parse(rDiagAll->body);
        assert(djAll.value("ok", false) == true);
        assert(djAll.contains("sessions") && djAll["sessions"].is_array() && !djAll["sessions"].empty());
    }

    // 3. Dynamically disable allow_cli_locked_detached_resume via POST /api/config and verify 503 block
    {
        json disablePatch = {
            {"gateway", {
                {"allow_cli_locked_detached_resume", false}
            }}
        };
        auto rPostCfg = client.Post("/api/config", headers, disablePatch.dump(), "application/json");
        assert(rPostCfg && rPostCfg->status == 200);

        auto rDiagBlocked = client.Get(("/api/agent/diagnostics?id=" + lockedCid).c_str(), headers);
        assert(rDiagBlocked && rDiagBlocked->status == 200);
        json djBlocked = json::parse(rDiagBlocked->body);
        assert(djBlocked.value("recommended_route", "") == "blocked_cli_locked_no_csrf");
        assert(djBlocked.value("interactive", true) == false);

        json promptReq = {
            {"agentId", "antigravity"},
            {"conversationId", lockedCid},
            {"text", "Should be blocked while Fallback 5 is disabled"}
        };
        auto rPromptBlocked = client.Post("/api/agent/prompt", headers, promptReq.dump(), "application/json");
        assert(rPromptBlocked && rPromptBlocked->status == 503);
        json pbj = json::parse(rPromptBlocked->body);
        assert(pbj.value("error", "").find("allow_cli_locked_detached_resume is disabled") != std::string::npos);
    }

    // 4. Re-enable allow_cli_locked_detached_resume + enable diagnostic logging via POST /api/config
    {
        json enablePatch = {
            {"gateway", {
                {"allow_cli_locked_detached_resume", true}
            }},
            {"diagnostics", {
                {"enabled", true},
                {"min_level", "INFO"},
                {"log_mobile_http", true},
                {"log_router_decisions", true}
            }}
        };
        auto rPostCfg = client.Post("/api/config", headers, enablePatch.dump(), "application/json");
        assert(rPostCfg && rPostCfg->status == 200);

        json promptReq = {
            {"agentId", "antigravity"},
            {"conversationId", lockedCid},
            {"text", "Resume locked terminal session via Fallback 5"}
        };
        auto rPromptOk = client.Post("/api/agent/prompt", headers, promptReq.dump(), "application/json");
        assert(rPromptOk && rPromptOk->status == 200);
        json poj = json::parse(rPromptOk->body);
        assert(poj.value("status", "") == "dispatched");
        assert(poj.value("routedVia", "") == "agy_cli_resume");

        // Verify mockAgyBin was invoked with --conversation <lockedCid> --dangerously-skip-permissions -p <text>
        std::ifstream af(agyArgsFile);
        std::string loggedArgs((std::istreambuf_iterator<char>(af)), std::istreambuf_iterator<char>());
        assert(loggedArgs.find("--conversation\n" + lockedCid) != std::string::npos);
        assert(loggedArgs.find("--dangerously-skip-permissions") != std::string::npos);
        assert(loggedArgs.find("Resume locked terminal session via Fallback 5") != std::string::npos);
    }

    // 5. Verify GET /api/agent/diagnostics/logs returns captured router_decision & platform_forward rows
    {
        auto rLogs = client.Get(("/api/agent/diagnostics/logs?id=" + lockedCid).c_str(), headers);
        assert(rLogs && rLogs->status == 200);
        json lj = json::parse(rLogs->body);
        assert(lj.value("ok", false) == true);
        assert(lj.contains("logs") && lj["logs"].is_array() && !lj["logs"].empty());
        bool sawRouter = false;
        bool sawForward = false;
        for (const auto& row : lj["logs"]) {
            std::string cat = row.value("category", "");
            if (cat == "router_decision") sawRouter = true;
            if (cat == "platform_forward") sawForward = true;
        }
        assert(sawRouter);
        assert(sawForward);
    }

    // 6. Verify POST /api/agent/diagnostics/clear purges debug logs and prunes stale presence lock files
    {
        assert(fs::exists(presenceDir + "/stale-dead-session.lock"));
        json clearReq = {
            {"purge_debug_data", true},
            {"clear_logs", true},
            {"prune_telemetry", true},
            {"prune_stale_locks", true},
            {"vacuum", true}
        };
        auto rClear = client.Post("/api/agent/diagnostics/clear", headers, clearReq.dump(), "application/json");
        assert(rClear && rClear->status == 200);
        json cj = json::parse(rClear->body);
        assert(cj.value("ok", false) == true);
        assert(cj.value("deleted_diagnostic_logs", 0) > 0);
        assert(cj.value("pruned_stale_locks", 0) == 1);
        assert(!fs::exists(presenceDir + "/stale-dead-session.lock"));
        assert(fs::exists(presenceDir + "/" + lockedCid + ".lock"));

        auto rLogsAfter = client.Get("/api/agent/diagnostics/logs", headers);
        assert(rLogsAfter && rLogsAfter->status == 200);
        json ljAfter = json::parse(rLogsAfter->body);
        assert(ljAfter["logs"].is_array() && ljAfter["logs"].empty());
    }

    // Restore DiagnosticsConfig defaults
    AgentTelemetryDb::getInstance().setDiagnosticsConfig(DiagnosticsConfig{});

    server.stop();
    unsetenv("AIMON_IDE_BRAIN_DIR");
    unsetenv("AIMON_CLI_BRAIN_DIR");
    unsetenv("AIMON_CLI_PRESENCE_DIR");
    unsetenv("AIMON_AGY_PROCESSES_FILE");
    unsetenv("AIMON_AGY_ENDPOINTS_FILE");
    unsetenv("AIMON_WORKSPACE_ROOT");
    unsetenv("AIMON_AGY_BIN");
    fs::remove_all(baseDir);
    std::cout << "[TestMobileGateway] RuntimeConfigAndDiagnosticsControlPlane_DynamicLockedCliFallbackAndPurge passed!" << std::endl;
}

static void testSessionScopedWorkspaceBrowsing() {
    std::cout << "[TestMobileGateway] Running testSessionScopedWorkspaceBrowsing..." << std::endl;
    std::string baseDir = "/tmp/aimon_sess_ws_" + std::to_string(getpid());
    fs::remove_all(baseDir);
    fs::create_directories(baseDir);

    std::string cliRoot = baseDir + "/cli_root";
    std::string cliBrain = cliRoot + "/brain";
    std::string ideBrain = baseDir + "/ide_brain";
    std::string dbPath = cliRoot + "/conversation_summaries.db";
    std::string customWs = baseDir + "/custom_ws";
    std::string tempMobileDb = baseDir + "/mobile_devices.json";

    fs::create_directories(cliBrain);
    fs::create_directories(ideBrain);
    fs::create_directories(customWs);

    // 1. Populate customWs with hello.cxx
    {
        std::ofstream hf(customWs + "/hello.cxx");
        hf << "#include <iostream>\nint main() { std::cout << \"Hello custom_ws\\n\"; return 0; }\n";
    }

    // 2. Create binary file data.bin with NUL byte
    {
        std::ofstream bf(customWs + "/data.bin", std::ios::binary);
        char binData[] = {'a', 'b', '\0', 'd'};
        bf.write(binData, sizeof(binData));
    }

    // 3. Create large file (>1 MB)
    {
        std::ofstream lf(customWs + "/large.txt");
        std::string chunk(1024, 'X');
        for (int i = 0; i < 1050; ++i) {
            lf << chunk;
        }
    }

    // 4. Create conversation_summaries.db with sess-with-ws and sess-no-ws
    {
        sqlite3* db = nullptr;
        assert(sqlite3_open(dbPath.c_str(), &db) == SQLITE_OK);
        const char* createSql =
            "CREATE TABLE conversation_summaries ("
            "conversation_id TEXT PRIMARY KEY, "
            "title TEXT NOT NULL DEFAULT '', "
            "preview TEXT NOT NULL DEFAULT '', "
            "step_count INTEGER NOT NULL DEFAULT 0, "
            "last_modified_time TEXT NOT NULL, "
            "workspace_uris TEXT NOT NULL DEFAULT '[]', "
            "status TEXT NOT NULL DEFAULT '', "
            "parent_conversation_id TEXT NOT NULL DEFAULT '', "
            "nesting_depth INTEGER NOT NULL DEFAULT 0, "
            "agent_name TEXT NOT NULL DEFAULT ''"
            ");";
        char* err = nullptr;
        assert(sqlite3_exec(db, createSql, nullptr, nullptr, &err) == SQLITE_OK);

        std::string insertWithWs =
            "INSERT INTO conversation_summaries "
            "(conversation_id, title, preview, step_count, last_modified_time, workspace_uris, status) "
            "VALUES ('sess-with-ws', 'Session With WS', 'Preview', 2, '2026-10-26 00:00:00', "
            "'[\"file://" + customWs + "\"]', 'CASCADE_RUN_STATUS_COMPLETED');";
        assert(sqlite3_exec(db, insertWithWs.c_str(), nullptr, nullptr, &err) == SQLITE_OK);

        const char* insertNoWs =
            "INSERT INTO conversation_summaries "
            "(conversation_id, title, preview, step_count, last_modified_time, workspace_uris, status) "
            "VALUES ('sess-no-ws', 'Session No WS', 'Preview', 2, '2026-10-26 00:00:00', "
            "'', 'CASCADE_RUN_STATUS_COMPLETED');";
        assert(sqlite3_exec(db, insertNoWs, nullptr, nullptr, &err) == SQLITE_OK);

        sqlite3_close(db);
    }

    // Also create transcript.jsonl for sess-with-ws in cliBrain
    {
        fs::path sessLogDir = fs::path(cliBrain) / "sess-with-ws" / ".system_generated" / "logs";
        fs::create_directories(sessLogDir);
        std::ofstream tf(sessLogDir / "transcript.jsonl");
        tf << "{\"step_index\":0,\"type\":\"USER_INPUT\",\"status\":\"DONE\",\"content\":\"<USER_REQUEST>\\nHi\\n</USER_REQUEST>\"}\n";
        tf << "{\"step_index\":1,\"type\":\"PLANNER_RESPONSE\",\"status\":\"DONE\",\"content\":\"Hello\",\"tool_calls\":[]}\n";
    }

    // Set test env (specifically unset AIMON_WORKSPACE_ROOT)
    unsetenv("AIMON_WORKSPACE_ROOT");
    setenv("AIMON_CLI_BRAIN_DIR", cliBrain.c_str(), 1);
    setenv("AIMON_IDE_BRAIN_DIR", ideBrain.c_str(), 1);

    assert(MobileGateway::getInstance().init(tempMobileDb));
    std::string secret = MobileGateway::getInstance().createPairingSecret(120);
    std::string validToken;
    assert(MobileGateway::getInstance().verifyPairingCode(secret, "sess-ws-dev", "Test Device", validToken));

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

    // 1. GET /api/workspace/tree (no conversationId): returns 200 with workspaceAvailable: false, workspace: "", tree: []
    auto resNoCid = client.Get("/api/workspace/tree", headers);
    assert(resNoCid && resNoCid->status == 200);
    json jNoCid = json::parse(resNoCid->body);
    assert(jNoCid.value("workspaceAvailable", true) == false);
    assert(jNoCid.value("workspace", "foo") == "");
    assert(jNoCid["tree"].is_array() && jNoCid["tree"].empty());
    assert(resNoCid->body.find("aimon") == std::string::npos);

    // 2. GET /api/workspace/tree?conversationId=new: returns 200 with workspaceAvailable: false
    auto resNew = client.Get("/api/workspace/tree?conversationId=new", headers);
    assert(resNew && resNew->status == 200);
    json jNew = json::parse(resNew->body);
    assert(jNew.value("workspaceAvailable", true) == false);
    assert(jNew.value("workspace", "foo") == "");
    assert(jNew["tree"].is_array() && jNew["tree"].empty());

    // 3. GET /api/workspace/tree?conversationId=nonexistent-session-id: returns 200 with workspaceAvailable: false
    auto resNonexistent = client.Get("/api/workspace/tree?conversationId=nonexistent-session-id", headers);
    assert(resNonexistent && resNonexistent->status == 200);
    json jNon = json::parse(resNonexistent->body);
    assert(jNon.value("workspaceAvailable", true) == false);
    assert(jNon["tree"].is_array() && jNon["tree"].empty());

    // 4. GET /api/workspace/tree?conversationId=sess-with-ws: returns workspaceAvailable: true, workspace: customWs, tree has hello.cxx
    auto resWithWs = client.Get("/api/workspace/tree?conversationId=sess-with-ws", headers);
    assert(resWithWs && resWithWs->status == 200);
    json jWithWs = json::parse(resWithWs->body);
    assert(jWithWs.value("workspaceAvailable", false) == true);
    assert(jWithWs.value("workspace", "") == customWs);
    assert(jWithWs["tree"].is_array() && !jWithWs["tree"].empty());
    bool foundHello = false;
    for (const auto& item : jWithWs["tree"]) {
        if (item.value("name", "") == "hello.cxx") {
            foundHello = true;
            break;
        }
    }
    assert(foundHello);

    // 5. GET /api/workspace/file?conversationId=sess-with-ws&path=hello.cxx reads hello.cxx
    auto resReadFile = client.Get("/api/workspace/file?conversationId=sess-with-ws&path=hello.cxx", headers);
    assert(resReadFile && resReadFile->status == 200);
    json jReadFile = json::parse(resReadFile->body);
    assert(jReadFile.value("path", "") == "hello.cxx");
    assert(jReadFile.value("content", "").find("Hello custom_ws") != std::string::npos);
    assert(jReadFile.value("language", "") == ".cxx");

    // 6. POST /api/workspace/file updates hello.cxx in custom_ws
    json postBody = {
        {"conversationId", "sess-with-ws"},
        {"path", "hello.cxx"},
        {"content", "// Updated content\nint main() { return 1; }\n"}
    };
    auto resWrite = client.Post("/api/workspace/file", headers, postBody.dump(), "application/json");
    assert(resWrite && resWrite->status == 200);
    json jWrite = json::parse(resWrite->body);
    assert(jWrite.value("status", "") == "ok");

    auto resReadUpdated = client.Get("/api/workspace/file?conversationId=sess-with-ws&path=hello.cxx", headers);
    assert(resReadUpdated && resReadUpdated->status == 200);
    json jReadUpdated = json::parse(resReadUpdated->body);
    assert(jReadUpdated.value("content", "").find("// Updated content") != std::string::npos);

    // 7. GET /api/workspace/tree?conversationId=sess-no-ws: returns workspaceAvailable: false, tree: []
    auto resNoWsTree = client.Get("/api/workspace/tree?conversationId=sess-no-ws", headers);
    assert(resNoWsTree && resNoWsTree->status == 200);
    json jNoWsTree = json::parse(resNoWsTree->body);
    assert(jNoWsTree.value("workspaceAvailable", true) == false);
    assert(jNoWsTree["tree"].is_array() && jNoWsTree["tree"].empty());

    // GET /api/workspace/file?conversationId=sess-no-ws&path=hello.cxx returns 404
    auto resNoWsFile = client.Get("/api/workspace/file?conversationId=sess-no-ws&path=hello.cxx", headers);
    assert(resNoWsFile && resNoWsFile->status == 404);

    // 8. Binary file guard: returns 415
    auto resBin = client.Get("/api/workspace/file?conversationId=sess-with-ws&path=data.bin", headers);
    assert(resBin && resBin->status == 415);

    // 9. Large file size guard (>1 MB): returns 413
    auto resLarge = client.Get("/api/workspace/file?conversationId=sess-with-ws&path=large.txt", headers);
    assert(resLarge && resLarge->status == 413);

    // 10. Directory traversal guard: returns 400
    auto resTrav = client.Get("/api/workspace/file?conversationId=sess-with-ws&path=../../etc/passwd", headers);
    assert(resTrav && resTrav->status == 400);

    // 11. /api/agent/available workspace scoping
    auto resAvailNoCid = client.Get("/api/agent/available", headers);
    assert(resAvailNoCid && resAvailNoCid->status == 200);
    json jAvailNoCid = json::parse(resAvailNoCid->body);
    assert(jAvailNoCid.value("current_workspace", "foo") == "");
    assert(jAvailNoCid["allowed_workspaces"].is_array() && jAvailNoCid["allowed_workspaces"].empty());
    assert(jAvailNoCid["agents"][0].value("workspaceRoot", "foo") == "");

    auto resAvailWithCid = client.Get("/api/agent/available?conversationId=sess-with-ws", headers);
    assert(resAvailWithCid && resAvailWithCid->status == 200);
    json jAvailWithCid = json::parse(resAvailWithCid->body);
    assert(jAvailWithCid.value("current_workspace", "") == customWs);
    assert(jAvailWithCid["allowed_workspaces"].size() == 1 && jAvailWithCid["allowed_workspaces"][0] == customWs);
    assert(jAvailWithCid["agents"][0].value("workspaceRoot", "") == customWs);

    // 12. /api/agent/transcript workspace scoping
    auto resTrans = client.Get("/api/agent/transcript?id=sess-with-ws", headers);
    assert(resTrans && resTrans->status == 200);
    json jTrans = json::parse(resTrans->body);
    assert(jTrans.value("workspacePath", "") == customWs);
    assert(jTrans.value("workspace", "") == fs::path(customWs).filename().string());

    // 13. Large workspace node budget capping: verify returned nodes <= 1000
    fs::create_directories(customWs + "/bigdir");
    for (int i = 0; i < 1050; ++i) {
        std::ofstream ef(customWs + "/bigdir/file_" + std::to_string(i) + ".txt");
        ef << i;
    }
    auto resBigTree = client.Get("/api/workspace/tree?conversationId=sess-with-ws", headers);
    assert(resBigTree && resBigTree->status == 200);
    json jBigTree = json::parse(resBigTree->body);
    assert(jBigTree.value("workspaceAvailable", false) == true);
    std::function<size_t(const json&)> countNodes = [&](const json& arr) -> size_t {
        size_t count = 0;
        for (const auto& item : arr) {
            count++;
            if (item.contains("children") && item["children"].is_array()) {
                count += countNodes(item["children"]);
            }
        }
        return count;
    };
    size_t returnedNodes = countNodes(jBigTree["tree"]);
    assert(returnedNodes <= 1000);

    server.stop();
    unsetenv("AIMON_CLI_BRAIN_DIR");
    unsetenv("AIMON_IDE_BRAIN_DIR");
    fs::remove_all(baseDir);
    std::cout << "[TestMobileGateway] testSessionScopedWorkspaceBrowsing passed!" << std::endl;
}

int main() {
    std::cout << "=== Starting TestMobileGateway ===" << std::endl;
    std::string testDbPath = "/tmp/test_mobile_gw_telemetry_" + std::to_string(getpid()) + ".db";
    AgentTelemetryDb::getInstance().open(testDbPath);
    testPairingAndAuth();
    testSessionPersistenceAndPermissions();
    testRapidCheckTokenValidation();
    testWebServerMobileAuth();
    testMultiRootBrainConversationDiscovery();
    testTranscriptJsonlParsingAndRapidCheck();
    testWorkspaceRecursiveTree();
    testSessionScopedWorkspaceBrowsing();
    testAgentPromptNoFakeElizaAndRealBridge();
    testAgentPromptPreservesCascadeConfigAndModelM418();
    testSlashCommandsAndSessionNotifications();
    testMultiProviderClaudeAndCursorSessionsAndNotifications();
    testSubagentFilterAskQuestionAndProviderSlashCommands();
    testOrphanSummaryFilteringAndZeroStepTerminalSession();
    testLiveScreenSessionRoutingAndIdeSessionProtection();
    Turn1CursorSessionWithSubagentsAndNoParentJsonl_AppearsInConversationsAndTranscript();
    NonInteractiveCursorAndClaudeIdeSessions_ReadOnlyInConversationsAndRejectedOnPromptWith409();
    FourHopDiagnosticTrace_CorrelatesRequestIdAndScrubsTokens();
    RuntimeConfigAndDiagnosticsControlPlane_DynamicLockedCliFallbackAndPurge();
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
