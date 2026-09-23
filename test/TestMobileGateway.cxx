/*
 * TestMobileGateway.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <cassert>
#include <iostream>
#include <thread>
#include <chrono>
#include "MobileGateway.hxx"
#include "AgentTelemetryDb.hxx"

using namespace aimon;

static void testPairingAndAuth() {
    std::cout << "[TestMobileGateway] Running testPairingAndAuth..." << std::endl;
    auto &gw = MobileGateway::getInstance();

    // 1. Create pairing secret
    std::string secret = gw.createPairingSecret(60);
    assert(!secret.empty());
    assert(secret.length() == 32); // 16 bytes hex

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

    std::cout << "[TestMobileGateway] testPairingAndAuth passed!" << std::endl;
}

static void testApprovalLatching() {
    std::cout << "[TestMobileGateway] Running testApprovalLatching..." << std::endl;
    auto &gw = MobileGateway::getInstance();

    nlohmann::json toolArgs;
    toolArgs["CommandLine"] = "make -j8";

    std::string approvalId = gw.submitApprovalRequest("antigravity", "run_command", "/workspace/aimon", toolArgs, "Build target", 5);
    assert(!approvalId.empty());

    // Check pending list
    auto pending = gw.listPendingApprovals();
    assert(pending.size() == 1);
    assert(pending[0]["approval_id"] == approvalId);
    assert(pending[0]["tool_name"] == "run_command");

    // Spawn background thread to simulate user approving from Android app
    std::thread approverThread([&gw, approvalId]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        assert(gw.resolveApproval(approvalId, ApprovalVerdict::APPROVED));
    });

    // Main thread waits on approval
    ApprovalVerdict verdict = gw.waitForApproval(approvalId, 5);
    assert(verdict == ApprovalVerdict::APPROVED);

    approverThread.join();

    // Re-resolving resolved approval should fail
    assert(!gw.resolveApproval(approvalId, ApprovalVerdict::APPROVED));

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

    std::cout << "[TestMobileGateway] testApprovalTimeout passed!" << std::endl;
}

int main() {
    std::cout << "=== Starting TestMobileGateway ===" << std::endl;
    testPairingAndAuth();
    testApprovalLatching();
    testApprovalTimeout();
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
