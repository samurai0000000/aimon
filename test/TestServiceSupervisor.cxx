/*
 * TestServiceSupervisor.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

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

#include <CppUTest/TestHarness.h>
#include <CppUTest/CommandLineTestRunner.h>

using namespace aimon;

TEST_GROUP(ServiceSupervisor_Unit) {
    void setup() override {
    }

    void teardown() override {
    }
};

TEST(ServiceSupervisor_Unit, StateToStringMapping) {
    STRCMP_EQUAL("HEALTHY", serviceStateToString(ServiceState::HEALTHY).c_str());
    STRCMP_EQUAL("DEGRADED", serviceStateToString(ServiceState::DEGRADED).c_str());
    STRCMP_EQUAL("RESTARTING", serviceStateToString(ServiceState::RESTARTING).c_str());
    STRCMP_EQUAL("CRASH_LOOP", serviceStateToString(ServiceState::CRASH_LOOP).c_str());
    STRCMP_EQUAL("DISABLED", serviceStateToString(ServiceState::DISABLED).c_str());
}

TEST(ServiceSupervisor_Unit, StatusToJsonSerialization) {
    ServiceRuntimeStatus status;
    status.config.id = "test-service";
    status.config.name = "Test Service";
    status.config.host = "127.0.0.1";
    status.config.port = 9000;
    status.config.secondaryPort = 9001;
    status.config.enabled = true;
    status.state = ServiceState::HEALTHY;
    status.probeLatencyMs = 12;
    status.consecutiveFailures = 0;
    status.restartCount = 2;
    status.recentRestartTimestamps = { 1000, 1020 };
    status.lastErrorMessage = "";

    nlohmann::json j = status.toJson();
    STRCMP_EQUAL("test-service", j["id"].get<std::string>().c_str());
    STRCMP_EQUAL("Test Service", j["name"].get<std::string>().c_str());
    STRCMP_EQUAL("127.0.0.1", j["host"].get<std::string>().c_str());
    LONGS_EQUAL(9000, j["port"].get<int>());
    LONGS_EQUAL(9001, j["secondary_port"].get<int>());
    CHECK_TRUE(j["enabled"].get<bool>());
    STRCMP_EQUAL("HEALTHY", j["state"].get<std::string>().c_str());
    LONGS_EQUAL(12, j["probe_latency_ms"].get<int>());
    LONGS_EQUAL(2, j["restart_count"].get<int>());
    LONGS_EQUAL(2, j["recent_restart_timestamps"].size());
}

TEST(ServiceSupervisor_Unit, StateTransitionMatrix_NominalAndDegraded) {
    SupervisorConfig supCfg;
    supCfg.enabled = true;
    supCfg.autoRestart = true;
    supCfg.crashLoopMaxRetries = 3;
    supCfg.crashLoopWindowSec = 60;

    std::vector<SupervisedServiceConfig> svcs;
    SupervisedServiceConfig svc;
    svc.id = "daemon-x";
    svc.name = "Daemon X";
    svc.host = "127.0.0.1";
    svc.port = 8888;
    svc.enabled = true;
    svcs.push_back(svc);

    ServiceSupervisor supervisor(supCfg, svcs);
    ServiceRuntimeStatus status;
    CHECK_TRUE(supervisor.getStatus("daemon-x", status));
    LONGS_EQUAL((int)ServiceState::HEALTHY, (int)status.state);

    // First failure -> DEGRADED
    supervisor.evaluateStateTransition(status, false, 0, "Connection refused");
    LONGS_EQUAL((int)ServiceState::DEGRADED, (int)status.state);
    LONGS_EQUAL(1, status.consecutiveFailures);

    // Second failure -> RESTARTING
    supervisor.evaluateStateTransition(status, false, 0, "Connection refused");
    LONGS_EQUAL((int)ServiceState::RESTARTING, (int)status.state);
    LONGS_EQUAL(2, status.consecutiveFailures);
    LONGS_EQUAL(1, status.restartCount);

    // Probe success -> back to HEALTHY, reset failures
    supervisor.evaluateStateTransition(status, true, 5, "");
    LONGS_EQUAL((int)ServiceState::HEALTHY, (int)status.state);
    LONGS_EQUAL(0, status.consecutiveFailures);
    LONGS_EQUAL(5, status.probeLatencyMs);
}

TEST(ServiceSupervisor_Unit, CircuitBreakerAntiThrashing) {
    SupervisorConfig supCfg;
    supCfg.enabled = true;
    supCfg.autoRestart = true;
    supCfg.crashLoopMaxRetries = 3;
    supCfg.crashLoopWindowSec = 60;

    std::vector<SupervisedServiceConfig> svcs;
    SupervisedServiceConfig svc;
    svc.id = "flapping-svc";
    svc.enabled = true;
    svcs.push_back(svc);

    ServiceSupervisor supervisor(supCfg, svcs);
    ServiceRuntimeStatus status;
    CHECK_TRUE(supervisor.getStatus("flapping-svc", status));

    // Simulate 3 restarts within window
    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    status.recentRestartTimestamps = { now - 10, now - 5, now - 1 };

    // Next failure causes transition to CRASH_LOOP
    status.consecutiveFailures = 2;
    supervisor.evaluateStateTransition(status, false, 0, "Repeated crash");
    LONGS_EQUAL((int)ServiceState::CRASH_LOOP, (int)status.state);

    // requestRestart without force should be rejected
    bool restarted = supervisor.requestRestart("flapping-svc", false);
    CHECK_FALSE(restarted);
}

TEST(ServiceSupervisor_Unit, DisabledServiceHandling) {
    SupervisorConfig supCfg;
    supCfg.enabled = true;

    std::vector<SupervisedServiceConfig> svcs;
    SupervisedServiceConfig svc;
    svc.id = "disabled-svc";
    svc.enabled = false;
    svcs.push_back(svc);

    ServiceSupervisor supervisor(supCfg, svcs);
    ServiceRuntimeStatus status;
    CHECK_TRUE(supervisor.getStatus("disabled-svc", status));
    LONGS_EQUAL((int)ServiceState::DISABLED, (int)status.state);

    supervisor.probeService(status);
    LONGS_EQUAL((int)ServiceState::DISABLED, (int)status.state);
}

// ---------------------------------------------------------------------------
// Integrated Test Fixture with Real Ephemeral Loopback TCP Listeners
// ---------------------------------------------------------------------------

class EphemeralTcpServer {
public:
    EphemeralTcpServer() : _listenFd(-1), _port(0), _running(false) {}

    ~EphemeralTcpServer() {
        stop();
    }

    bool start() {
        _listenFd = socket(AF_INET, SOCK_STREAM, 0);
        if (_listenFd < 0) {
            return false;
        }

        int opt = 1;
        setsockopt(_listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1
        addr.sin_port = 0; // Kernel picks ephemeral port

        if (bind(_listenFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(_listenFd);
            _listenFd = -1;
            return false;
        }

        socklen_t len = sizeof(addr);
        if (getsockname(_listenFd, (struct sockaddr*)&addr, &len) < 0) {
            close(_listenFd);
            _listenFd = -1;
            return false;
        }
        _port = ntohs(addr.sin_port);

        if (listen(_listenFd, 5) < 0) {
            close(_listenFd);
            _listenFd = -1;
            return false;
        }

        _running.store(true);
        _worker = std::make_unique<std::thread>([this]() {
            while (_running.load()) {
                struct timeval tv;
                tv.tv_sec = 0;
                tv.tv_usec = 50000; // 50ms

                fd_set readFds;
                FD_ZERO(&readFds);
                FD_SET(_listenFd, &readFds);

                int rc = select(_listenFd + 1, &readFds, nullptr, nullptr, &tv);
                if (rc > 0 && FD_ISSET(_listenFd, &readFds)) {
                    struct sockaddr_in clientAddr;
                    socklen_t clientLen = sizeof(clientAddr);
                    int clientFd = accept(_listenFd, (struct sockaddr*)&clientAddr, &clientLen);
                    if (clientFd >= 0) {
                        // Accept connection and close it cleanly
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
        }
        _worker.reset();
        if (_listenFd >= 0) {
            close(_listenFd);
            _listenFd = -1;
        }
    }

    int getPort() const {
        return _port;
    }

private:
    int _listenFd;
    int _port;
    std::atomic<bool> _running;
    std::unique_ptr<std::thread> _worker;
};

TEST_GROUP(ServiceSupervisor_Integrated) {
    void setup() override {
    }

    void teardown() override {
    }
};

TEST(ServiceSupervisor_Integrated, ProbeTcpPort_SuccessOnLoopbackListener) {
    EphemeralTcpServer server;
    CHECK_TRUE(server.start());
    int port = server.getPort();
    CHECK_TRUE(port > 0);

    int latency = 0;
    std::string err;
    bool success = ServiceSupervisor::probeTcpPort("127.0.0.1", port, 1000, latency, err);
    CHECK_TRUE(success);
    CHECK_TRUE(latency >= 0);
    STRCMP_EQUAL("", err.c_str());

    server.stop();
}

TEST(ServiceSupervisor_Integrated, ProbeTcpPort_ConnectionRefused) {
    // Acquire and immediately close a port to ensure connection refused
    int tempFd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK_TRUE(tempFd >= 0);
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    CHECK_TRUE(bind(tempFd, (struct sockaddr*)&addr, sizeof(addr)) == 0);

    socklen_t len = sizeof(addr);
    CHECK_TRUE(getsockname(tempFd, (struct sockaddr*)&addr, &len) == 0);
    int closedPort = ntohs(addr.sin_port);
    close(tempFd);

    int latency = 0;
    std::string err;
    bool success = ServiceSupervisor::probeTcpPort("127.0.0.1", closedPort, 500, latency, err);
    CHECK_FALSE(success);
    CHECK_TRUE(!err.empty());
}

TEST(ServiceSupervisor_Integrated, ProbeTcpPort_InvalidPortAndTimeout) {
    int latency = 0;
    std::string err;

    // Boundary: Port 0
    bool res0 = ServiceSupervisor::probeTcpPort("127.0.0.1", 0, 500, latency, err);
    CHECK_FALSE(res0);
    CHECK_TRUE(err.find("Invalid port") != std::string::npos);

    // Boundary: Port > 65535
    bool resOver = ServiceSupervisor::probeTcpPort("127.0.0.1", 70000, 500, latency, err);
    CHECK_FALSE(resOver);
    CHECK_TRUE(err.find("Invalid port") != std::string::npos);

    // Boundary: Unreachable non-routable IP with short timeout (100ms)
    bool resTimeout = ServiceSupervisor::probeTcpPort("192.0.2.1", 80, 100, latency, err);
    CHECK_FALSE(resTimeout);
}

TEST(ServiceSupervisor_Integrated, SecondaryPortFallback_Integrated) {
    // Primary port is closed
    int tempFd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    bind(tempFd, (struct sockaddr*)&addr, sizeof(addr));
    socklen_t len = sizeof(addr);
    getsockname(tempFd, (struct sockaddr*)&addr, &len);
    int primaryClosedPort = ntohs(addr.sin_port);
    close(tempFd);

    // Secondary port is open
    EphemeralTcpServer secondaryServer;
    CHECK_TRUE(secondaryServer.start());
    int secondaryOpenPort = secondaryServer.getPort();

    SupervisorConfig supCfg;
    supCfg.enabled = true;
    supCfg.probeTimeoutMs = 500;

    std::vector<SupervisedServiceConfig> svcs;
    SupervisedServiceConfig svc;
    svc.id = "meshmon-test";
    svc.host = "127.0.0.1";
    svc.port = primaryClosedPort;
    svc.secondaryPort = secondaryOpenPort;
    svc.enabled = true;
    svcs.push_back(svc);

    ServiceSupervisor supervisor(supCfg, svcs);
    ServiceRuntimeStatus status;
    CHECK_TRUE(supervisor.getStatus("meshmon-test", status));

    supervisor.probeService(status);
    LONGS_EQUAL((int)ServiceState::HEALTHY, (int)status.state);
    CHECK_TRUE(status.lastErrorMessage.find("secondary port") != std::string::npos);

    secondaryServer.stop();
}

TEST(ServiceSupervisor_Integrated, AsynchronousRestartDispatch) {
    std::string markerPath = "/tmp/aimon_test_restart_marker_" + std::to_string(getpid()) + ".txt";
    std::filesystem::remove(markerPath);

    SupervisorConfig supCfg;
    supCfg.enabled = true;
    supCfg.autoRestart = true;

    std::vector<SupervisedServiceConfig> svcs;
    SupervisedServiceConfig svc;
    svc.id = "restart-test";
    svc.host = "127.0.0.1";
    svc.port = 12345;
    svc.enabled = true;
    svc.startCmd = "touch " + markerPath;
    svcs.push_back(svc);

    ServiceSupervisor supervisor(supCfg, svcs);
    bool req = supervisor.requestRestart("restart-test", true);
    CHECK_TRUE(req);

    // Wait up to 1 second for child process to execute
    bool markerCreated = false;
    for (int i = 0; i < 20; ++i) {
        if (std::filesystem::exists(markerPath)) {
            markerCreated = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK_TRUE(markerCreated);
    std::filesystem::remove(markerPath);
}

TEST(ServiceSupervisor_Integrated, FullSupervisorLifecycleAndWorkerThread) {
    EphemeralTcpServer server;
    CHECK_TRUE(server.start());

    SupervisorConfig supCfg;
    supCfg.enabled = true;
    supCfg.probeIntervalSec = 1;
    supCfg.probeTimeoutMs = 200;

    std::vector<SupervisedServiceConfig> svcs;
    SupervisedServiceConfig svc;
    svc.id = "live-svc";
    svc.host = "127.0.0.1";
    svc.port = server.getPort();
    svc.enabled = true;
    svcs.push_back(svc);

    ServiceSupervisor supervisor(supCfg, svcs);
    CHECK_TRUE(supervisor.start());
    CHECK_TRUE(supervisor.isRunning());

    // Allow worker loop to run at least one probe
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    ServiceRuntimeStatus status;
    CHECK_TRUE(supervisor.getStatus("live-svc", status));
    LONGS_EQUAL((int)ServiceState::HEALTHY, (int)status.state);

    supervisor.stop();
    CHECK_FALSE(supervisor.isRunning());
    server.stop();
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
