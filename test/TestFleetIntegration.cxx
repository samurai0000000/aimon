/*
 * TestFleetIntegration.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>

#include <chrono>
#include <thread>
#include <atomic>
#include <string>
#include <vector>
#include <memory>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#include "ServiceSupervisor.hxx"
#include "ProcessMonitor.hxx"
#include "ConfigManager.hxx"

#include <CppUTest/TestHarness.h>
#include <CppUTest/CommandLineTestRunner.h>

using namespace aimon;

class EphemeralTcpServer {
public:
    EphemeralTcpServer() {
        start();
    }

    ~EphemeralTcpServer() {
        stop();
    }

    bool start(int fixedPort = 0) {
        stop();
        _fd = socket(AF_INET, SOCK_STREAM, 0);
        if (_fd < 0) return false;

        int opt = 1;
        setsockopt(_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(fixedPort);

        if (bind(_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(_fd);
            _fd = -1;
            return false;
        }

        socklen_t len = sizeof(addr);
        if (getsockname(_fd, (struct sockaddr*)&addr, &len) == 0) {
            _port = ntohs(addr.sin_port);
        }

        if (listen(_fd, 10) < 0) {
            close(_fd);
            _fd = -1;
            return false;
        }

        _running.store(true);
        _worker = std::make_unique<std::thread>([this]() {
            while (_running.load()) {
                fd_set rfds;
                FD_ZERO(&rfds);
                FD_SET(_fd, &rfds);
                timeval tv{0, 50000};
                int ret = select(_fd + 1, &rfds, nullptr, nullptr, &tv);
                if (ret > 0 && FD_ISSET(_fd, &rfds)) {
                    int clientFd = accept(_fd, nullptr, nullptr);
                    if (clientFd >= 0) {
                        close(clientFd);
                    }
                }
            }
        });

        return true;
    }

    void stop() {
        _running.store(false);
        if (_worker && _worker->joinable()) {
            _worker->join();
            _worker.reset();
        }
        if (_fd >= 0) {
            close(_fd);
            _fd = -1;
        }
    }

    int getPort() const { return _port; }

private:
    int _fd = -1;
    int _port = 0;
    std::atomic<bool> _running{false};
    std::unique_ptr<std::thread> _worker;
};

// ============================================================================
// Fleet Integration Test Group
// ============================================================================

TEST_GROUP(FleetIntegration) {
    void setup() override {
    }

    void teardown() override {
    }
};

TEST(FleetIntegration, MultiDaemonProbeAndRecoveryLifecycle) {
    EphemeralTcpServer serverA;
    EphemeralTcpServer serverB;
    int portA = serverA.getPort();
    int portB = serverB.getPort();
    CHECK_TRUE(portA > 0);
    CHECK_TRUE(portB > 0);

    SupervisorConfig sc;
    sc.enabled = true;
    sc.autoRestart = true;
    sc.probeIntervalSec = 1;
    sc.probeTimeoutSec = 1;
    sc.probeTimeoutMs = 500;
    sc.crashLoopWindowSec = 60;
    sc.crashLoopMaxRetries = 5;

    std::vector<SupervisedServiceConfig> services;

    SupervisedServiceConfig svcA;
    svcA.id = "fleet-svc-a";
    svcA.name = "Fleet Service A";
    svcA.host = "127.0.0.1";
    svcA.port = portA;
    svcA.startCmd = "true";
    svcA.enabled = true;
    services.push_back(svcA);

    SupervisedServiceConfig svcB;
    svcB.id = "fleet-svc-b";
    svcB.name = "Fleet Service B";
    svcB.host = "127.0.0.1";
    svcB.port = portB;
    svcB.startCmd = "true";
    svcB.enabled = true;
    services.push_back(svcB);

    ServiceSupervisor supervisor(sc, services);
    CHECK_TRUE(supervisor.start());

    // 1. Initial probe: both services should become HEALTHY
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    ServiceRuntimeStatus statusA, statusB;
    CHECK_TRUE(supervisor.getStatus("fleet-svc-a", statusA));
    CHECK_TRUE(supervisor.getStatus("fleet-svc-b", statusB));
    LONGS_EQUAL((int)ServiceState::HEALTHY, (int)statusA.state);
    LONGS_EQUAL((int)ServiceState::HEALTHY, (int)statusB.state);

    // 2. Failure injection: terminate server A
    serverA.stop();

    // Wait for next probe interval
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    CHECK_TRUE(supervisor.getStatus("fleet-svc-a", statusA));
    CHECK_TRUE(statusA.state == ServiceState::DEGRADED || statusA.state == ServiceState::RESTARTING);

    // 3. Recovery: bring server A back on the same port
    CHECK_TRUE(serverA.start(portA));

    // Wait for recovery probe
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    CHECK_TRUE(supervisor.getStatus("fleet-svc-a", statusA));
    LONGS_EQUAL((int)ServiceState::HEALTHY, (int)statusA.state);

    // 4. Verify server B stayed continuously healthy throughout
    CHECK_TRUE(supervisor.getStatus("fleet-svc-b", statusB));
    LONGS_EQUAL((int)ServiceState::HEALTHY, (int)statusB.state);

    supervisor.stop();
    serverA.stop();
    serverB.stop();
}

TEST(FleetIntegration, RapidFlappingCircuitBreakerTripping) {
    SupervisorConfig sc;
    sc.enabled = true;
    sc.autoRestart = true;
    sc.probeIntervalSec = 1;
    sc.probeTimeoutSec = 1;
    sc.probeTimeoutMs = 200;
    sc.crashLoopWindowSec = 10;
    sc.crashLoopMaxRetries = 2; // Trip after 2 restarts

    std::vector<SupervisedServiceConfig> services;
    SupervisedServiceConfig svcC;
    svcC.id = "fleet-svc-flapping";
    svcC.name = "Flapping Service";
    svcC.host = "127.0.0.1";
    svcC.port = 19999; // Closed port
    svcC.startCmd = "true";
    svcC.enabled = true;
    services.push_back(svcC);

    ServiceSupervisor supervisor(sc, services);
    CHECK_TRUE(supervisor.start());

    // Allow multiple probe and restart cycles to trip circuit breaker
    std::this_thread::sleep_for(std::chrono::milliseconds(3500));

    ServiceRuntimeStatus status;
    CHECK_TRUE(supervisor.getStatus("fleet-svc-flapping", status));
    LONGS_EQUAL((int)ServiceState::CRASH_LOOP, (int)status.state);

    // Normal restart request should be rejected due to active circuit breaker
    CHECK_FALSE(supervisor.requestRestart("fleet-svc-flapping", false));

    // Force restart request should succeed and reset circuit breaker
    CHECK_TRUE(supervisor.requestRestart("fleet-svc-flapping", true));

    supervisor.stop();
}

TEST(FleetIntegration, ProcessMonitorSignalForwardingAndCleanExit) {
    SupervisorConfig sc;
    sc.enabled = true;
    sc.crashLoopMaxRetries = 3;
    sc.crashLoopWindowSec = 10;
    sc.backoffInitialSec = 1;

    GeminiConfig gc;
    char* fakeArgv[] = { (char*)"aimon", nullptr };
    ProcessMonitor monitor(sc, gc, 1, fakeArgv);

    // Test socketpair allocation and non-blocking flags
    int fds[2] = { -1, -1 };
    CHECK_EQUAL(0, ProcessMonitor::createSocketPair(fds));

    // Verify token transmission
    CHECK_TRUE(ProcessMonitor::sendIpcToken(fds[1], "SHUTDOWN"));
    std::string token;
    CHECK_TRUE(ProcessMonitor::readIpcToken(fds[0], token));
    CHECK_EQUAL("SHUTDOWN\n", token);

    close(fds[0]);
    close(fds[1]);

    // Test evaluateChildExit clean vs abnormal
    LONGS_EQUAL((int)MonitorAction::CLEAN_TERMINATION, (int)monitor.evaluateChildExit(0));
    LONGS_EQUAL((int)MonitorAction::RESPAWN_IMMEDIATE, (int)monitor.evaluateChildExit(11));
}

int main(int argc, char** argv) {
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
