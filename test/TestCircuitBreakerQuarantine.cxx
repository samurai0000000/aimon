/*
 * TestCircuitBreakerQuarantine.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "ProcessMonitor.hxx"
#include <CppUTest/TestHarness.h>
#include <CppUTest/CommandLineTestRunner.h>

namespace fs = std::filesystem;
using namespace aimon;

TEST_GROUP(CircuitBreaker_Unit) {
    void setup() override {
    }

    void teardown() override {
    }
};

TEST(CircuitBreaker_Unit, ExponentialBackoffCalculation) {
    SupervisorConfig supCfg;
    supCfg.backoffInitialSec = 2;
    supCfg.backoffMaxSec = 30;
    GeminiConfig gemCfg;
    char* dummyArgv[] = { (char*)"aimon", nullptr };
    ProcessMonitor monitor(supCfg, gemCfg, 1, dummyArgv);

    CHECK_EQUAL(0, monitor.calculateBackoffSec());

    // Simulate crashes and check backoff progression
    // Crash 1: 2 * (1 << 0) = 2
    monitor.evaluateChildExit(1);
    CHECK_EQUAL(2, monitor.calculateBackoffSec());

    // Crash 2: 2 * (1 << 1) = 4
    monitor.evaluateChildExit(1);
    CHECK_EQUAL(4, monitor.calculateBackoffSec());

    // Crash 3: 2 * (1 << 2) = 8
    monitor.evaluateChildExit(1);
    CHECK_EQUAL(8, monitor.calculateBackoffSec());

    // Crash 4: 2 * (1 << 3) = 16
    monitor.evaluateChildExit(1);
    CHECK_EQUAL(16, monitor.calculateBackoffSec());

    // Crash 5: 2 * (1 << 4) = 32, capped at 30
    monitor.evaluateChildExit(1);
    CHECK_EQUAL(30, monitor.calculateBackoffSec());
}

TEST(CircuitBreaker_Unit, SlidingWindowTimestampPruning) {
    SupervisorConfig supCfg;
    supCfg.crashLoopWindowSec = 60;
    GeminiConfig gemCfg;
    char* dummyArgv[] = { (char*)"aimon", nullptr };
    ProcessMonitor monitor(supCfg, gemCfg, 1, dummyArgv);

    // Simulate crash 1
    monitor.evaluateChildExit(1);
    CHECK_EQUAL(1, static_cast<int>(monitor.getActiveCrashCount()));

    // Pruning with current time keeps the timestamp
    monitor.pruneCrashTimestamps(time(nullptr));
    CHECK_EQUAL(1, static_cast<int>(monitor.getActiveCrashCount()));

    // Pruning with future time past the 60s window removes the timestamp
    monitor.pruneCrashTimestamps(time(nullptr) + 120);
    CHECK_EQUAL(0, static_cast<int>(monitor.getActiveCrashCount()));
}

TEST_GROUP(CircuitBreaker_Integrated) {
    std::string _tempDir;

    void setup() override {
        _tempDir = "/tmp/aimon_cb_test_" + std::to_string(getpid());
        fs::create_directories(_tempDir);
    }

    void teardown() override {
        if (fs::exists(_tempDir)) {
            fs::remove_all(_tempDir);
        }
    }
};

TEST(CircuitBreaker_Integrated, AtomicDbQuarantineAndPrune) {
    std::string db1 = _tempDir + "/history.db";
    std::string db1Wal = _tempDir + "/history.db-wal";
    std::string db2 = _tempDir + "/agent_telemetry.db";

    std::ofstream(db1) << "dummy history database";
    std::ofstream(db1Wal) << "dummy wal";
    std::ofstream(db2) << "dummy telemetry database";

    SupervisorConfig supCfg;
    supCfg.maxDbQuarantineVersions = 2;
    GeminiConfig gemCfg;
    char* dummyArgv[] = { (char*)"aimon", nullptr };
    ProcessMonitor monitor(supCfg, gemCfg, 1, dummyArgv);

    bool quarantined = monitor.executeDbQuarantine(_tempDir);
    CHECK_TRUE(quarantined);

    // Assert originals no longer in root
    CHECK_FALSE(fs::exists(db1));
    CHECK_FALSE(fs::exists(db1Wal));
    CHECK_FALSE(fs::exists(db2));

    // Assert quarantine directory created and contains files
    std::string qBase = _tempDir + "/quarantine";
    CHECK_TRUE(fs::exists(qBase));

    int dirCount = 0;
    for (const auto& entry : fs::directory_iterator(qBase)) {
        if (entry.is_directory()) {
            dirCount++;
            CHECK_TRUE(fs::exists(entry.path() / "history.db"));
            CHECK_TRUE(fs::exists(entry.path() / "agent_telemetry.db"));
        }
    }
    CHECK_EQUAL(1, dirCount);

    // Trigger multiple quarantines to test pruning
    for (int i = 0; i < 4; ++i) {
        usleep(1100000); // 1.1s to get distinct second timestamps
        std::ofstream(_tempDir + "/history.db") << "dummy db " << i;
        monitor.executeDbQuarantine(_tempDir);
    }

    // Check directory count does not exceed maxDbQuarantineVersions (2)
    dirCount = 0;
    for (const auto& entry : fs::directory_iterator(qBase)) {
        if (entry.is_directory()) {
            dirCount++;
        }
    }
    CHECK_EQUAL(2, dirCount);
}

TEST(CircuitBreaker_Integrated, CircuitBreakerStateTransitions) {
    SupervisorConfig supCfg;
    supCfg.crashLoopMaxRetries = 3;
    supCfg.crashLoopWindowSec = 60;
    supCfg.prevBinaryPath = ""; // No rollback binary
    GeminiConfig gemCfg;
    char* dummyArgv[] = { (char*)"aimon", nullptr };
    ProcessMonitor monitor(supCfg, gemCfg, 1, dummyArgv);

    // Crash 1: Normal respawn
    MonitorAction action1 = monitor.evaluateChildExit(1);
    CHECK_TRUE(action1 == MonitorAction::RESPAWN_IMMEDIATE);
    CHECK_FALSE(monitor.isSafeModeActive());

    // Crash 2: Normal respawn
    MonitorAction action2 = monitor.evaluateChildExit(1);
    CHECK_TRUE(action2 == MonitorAction::RESPAWN_IMMEDIATE);
    CHECK_FALSE(monitor.isSafeModeActive());

    // Crash 3: Hits threshold (3) -> Activates safe mode & DB quarantine
    MonitorAction action3 = monitor.evaluateChildExit(1);
    CHECK_TRUE(action3 == MonitorAction::RESPAWN_IMMEDIATE);
    CHECK_TRUE(monitor.isSafeModeActive());

    // Crash 4 (while in safe mode with no rollback binary) -> Trips circuit breaker completely!
    MonitorAction action4 = monitor.evaluateChildExit(1);
    CHECK_TRUE(action4 == MonitorAction::TRIP_CIRCUIT_BREAKER);
}

TEST(CircuitBreaker_Integrated, RollbackBinaryFallback) {
    std::string fakePrevBin = _tempDir + "/aimon.prev";
    std::ofstream out(fakePrevBin);
    out << "#!/bin/sh\nexit 0\n";
    out.close();
    chmod(fakePrevBin.c_str(), 0755);

    SupervisorConfig supCfg;
    supCfg.crashLoopMaxRetries = 3;
    supCfg.crashLoopWindowSec = 60;
    supCfg.prevBinaryPath = fakePrevBin;
    GeminiConfig gemCfg;
    char* dummyArgv[] = { (char*)"aimon", nullptr };
    ProcessMonitor monitor(supCfg, gemCfg, 1, dummyArgv);

    // Force monitor into safe mode
    monitor.setSafeModeActive(true);

    // Fill crash window to trigger next fallback stage
    monitor.evaluateChildExit(1);
    monitor.evaluateChildExit(1);
    MonitorAction action = monitor.evaluateChildExit(1);

    // Since safe mode is already active and prevBinaryPath exists and is executable,
    // it rolls back to the previous binary!
    CHECK_TRUE(action == MonitorAction::RESPAWN_IMMEDIATE);
    CHECK_TRUE(monitor.isRollbackActive());

    // If rollback binary also crashes, circuit breaker trips
    MonitorAction finalAction = monitor.evaluateChildExit(1);
    CHECK_TRUE(finalAction == MonitorAction::TRIP_CIRCUIT_BREAKER);
}

int main(int ac, char** av) {
    return CommandLineTestRunner::RunAllTests(ac, av);
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
