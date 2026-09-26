/*
 * TestProcessMonitorLifecycle.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <poll.h>
#include <csignal>
#include "ProcessMonitor.hxx"
#include <CppUTest/TestHarness.h>
#include <CppUTest/CommandLineTestRunner.h>

using namespace aimon;

TEST_GROUP(ProcessMonitor_Unit) {
    void setup() override {
    }

    void teardown() override {
        ProcessMonitor::setChildIpcFd(-1);
    }
};

TEST(ProcessMonitor_Unit, SocketPairCreation) {
    int fds[2] = { -1, -1 };
    int ret = ProcessMonitor::createSocketPair(fds);
    CHECK_EQUAL(0, ret);
    CHECK_TRUE(fds[0] >= 0);
    CHECK_TRUE(fds[1] >= 0);

    // Verify non-blocking flag
    int flags0 = fcntl(fds[0], F_GETFL, 0);
    int flags1 = fcntl(fds[1], F_GETFL, 0);
    CHECK_TRUE((flags0 & O_NONBLOCK) != 0);
    CHECK_TRUE((flags1 & O_NONBLOCK) != 0);

    close(fds[0]);
    close(fds[1]);
}

TEST(ProcessMonitor_Unit, IpcTokenTransmission) {
    int fds[2] = { -1, -1 };
    CHECK_EQUAL(0, ProcessMonitor::createSocketPair(fds));

    bool sendOk = ProcessMonitor::sendIpcToken(fds[1], "SHUTDOWN");
    CHECK_TRUE(sendOk);

    std::string token;
    bool readOk = ProcessMonitor::readIpcToken(fds[0], token);
    CHECK_TRUE(readOk);
    CHECK_EQUAL("SHUTDOWN\n", token);

    close(fds[0]);
    close(fds[1]);
}

TEST(ProcessMonitor_Unit, WorkerShutdownNotification) {
    int fds[2] = { -1, -1 };
    CHECK_EQUAL(0, ProcessMonitor::createSocketPair(fds));

    ProcessMonitor::setChildIpcFd(fds[1]);
    ProcessMonitor::notifyWorkerShutdown();

    std::string token;
    bool readOk = ProcessMonitor::readIpcToken(fds[0], token);
    CHECK_TRUE(readOk);
    CHECK_EQUAL("SHUTDOWN\n", token);

    close(fds[0]);
    close(fds[1]);
}

TEST(ProcessMonitor_Unit, EvaluateCleanExitStatus) {
    SupervisorConfig supCfg;
    GeminiConfig gemCfg;
    char* dummyArgv[] = { (char*)"aimon", nullptr };
    ProcessMonitor monitor(supCfg, gemCfg, 1, dummyArgv);

    // Simulate status for normal exit code 0
    int normalStatus = 0;
    MonitorAction action = monitor.evaluateChildExit(normalStatus);
    CHECK_TRUE(action == MonitorAction::CLEAN_TERMINATION);
}

TEST(ProcessMonitor_Unit, EvaluateAbnormalExitStatus) {
    SupervisorConfig supCfg;
    GeminiConfig gemCfg;
    char* dummyArgv[] = { (char*)"aimon", nullptr };
    ProcessMonitor monitor(supCfg, gemCfg, 1, dummyArgv);

    // Simulate status for exit code 1: (1 << 8)
    int exitCode1Status = (1 << 8);
    MonitorAction actionExit1 = monitor.evaluateChildExit(exitCode1Status);
    CHECK_TRUE(actionExit1 == MonitorAction::RESPAWN_IMMEDIATE);

    // Simulate status for SIGSEGV (signal 11)
    int sigsegvStatus = 11;
    MonitorAction actionSigsegv = monitor.evaluateChildExit(sigsegvStatus);
    CHECK_TRUE(actionSigsegv == MonitorAction::RESPAWN_IMMEDIATE);

    const auto& incidents = monitor.getIncidentHistory();
    CHECK_EQUAL(2, static_cast<int>(incidents.size()));
    CHECK_EQUAL(1, incidents[0].exitStatus);
    CHECK_EQUAL(0, incidents[0].termSignal);
    CHECK_EQUAL(0, incidents[1].exitStatus);
    CHECK_EQUAL(11, incidents[1].termSignal);
}

TEST_GROUP(ProcessMonitor_Integrated) {
    void setup() override {
    }

    void teardown() override {
        ProcessMonitor::setChildIpcFd(-1);
    }
};

TEST(ProcessMonitor_Integrated, HermeticCleanShutdownSymmetry) {
    int fds[2] = { -1, -1 };
    CHECK_EQUAL(0, ProcessMonitor::createSocketPair(fds));

    pid_t childPid = fork();
    CHECK_TRUE(childPid >= 0);

    if (childPid == 0) {
        // Hermetic test child
        close(fds[0]);
        ProcessMonitor::setChildIpcFd(fds[1]);
        ProcessMonitor::notifyWorkerShutdown();
        close(fds[1]);
        _exit(0);
    }

    // Parent
    close(fds[1]);
    std::string token;
    bool readOk = false;
    for (int retry = 0; retry < 50; ++retry) {
        if (ProcessMonitor::readIpcToken(fds[0], token)) {
            readOk = true;
            break;
        }
        usleep(10000);
    }
    CHECK_TRUE(readOk);
    CHECK_EQUAL("SHUTDOWN\n", token);

    int status = 0;
    pid_t reaped = waitpid(childPid, &status, 0);
    CHECK_EQUAL(childPid, reaped);
    CHECK_TRUE(WIFEXITED(status));
    CHECK_EQUAL(0, WEXITSTATUS(status));

    SupervisorConfig supCfg;
    GeminiConfig gemCfg;
    char* dummyArgv[] = { (char*)"aimon", nullptr };
    ProcessMonitor monitor(supCfg, gemCfg, 1, dummyArgv);
    MonitorAction action = monitor.evaluateChildExit(status);
    CHECK_TRUE(action == MonitorAction::CLEAN_TERMINATION);

    close(fds[0]);
}

static void testSignalHandler(int sig) {
    (void)sig;
    ProcessMonitor::notifyWorkerShutdown();
    _exit(0);
}

TEST(ProcessMonitor_Integrated, HermeticSignalForwardingToTestChild) {
    int fds[2] = { -1, -1 };
    CHECK_EQUAL(0, ProcessMonitor::createSocketPair(fds));

    pid_t childPid = fork();
    CHECK_TRUE(childPid >= 0);

    if (childPid == 0) {
        close(fds[0]);
        ProcessMonitor::setChildIpcFd(fds[1]);
        struct sigaction sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sa_handler = testSignalHandler;
        sigaction(SIGTERM, &sa, nullptr);

        // Wait for parent signal
        while (true) {
            pause();
        }
        _exit(1);
    }

    close(fds[1]);
    // Allow child to install signal handler
    usleep(20000);

    // Send SIGTERM to the hermetic test child
    kill(childPid, SIGTERM);

    std::string token;
    bool readOk = false;
    for (int retry = 0; retry < 50; ++retry) {
        if (ProcessMonitor::readIpcToken(fds[0], token)) {
            readOk = true;
            break;
        }
        usleep(10000);
    }
    CHECK_TRUE(readOk);
    CHECK_EQUAL("SHUTDOWN\n", token);

    int status = 0;
    pid_t reaped = waitpid(childPid, &status, 0);
    CHECK_EQUAL(childPid, reaped);
    CHECK_TRUE(WIFEXITED(status));
    CHECK_EQUAL(0, WEXITSTATUS(status));

    close(fds[0]);
}

TEST(ProcessMonitor_Integrated, HermeticChildCrashDetection) {
    pid_t childPid = fork();
    CHECK_TRUE(childPid >= 0);

    if (childPid == 0) {
        // Child raises SIGSEGV to simulate abnormal crash
        kill(getpid(), SIGSEGV);
        _exit(1);
    }

    int status = 0;
    pid_t reaped = waitpid(childPid, &status, 0);
    CHECK_EQUAL(childPid, reaped);
    CHECK_TRUE(WIFSIGNALED(status));
    CHECK_EQUAL(SIGSEGV, WTERMSIG(status));

    SupervisorConfig supCfg;
    GeminiConfig gemCfg;
    char* dummyArgv[] = { (char*)"aimon", nullptr };
    ProcessMonitor monitor(supCfg, gemCfg, 1, dummyArgv);

    MonitorAction action = monitor.evaluateChildExit(status);
    CHECK_TRUE(action == MonitorAction::RESPAWN_IMMEDIATE);
    CHECK_EQUAL(1, static_cast<int>(monitor.getIncidentHistory().size()));
    CHECK_EQUAL(SIGSEGV, monitor.getIncidentHistory().back().termSignal);
}

TEST(ProcessMonitor_Integrated, BoundaryPartialTokenStream) {
    int fds[2] = { -1, -1 };
    CHECK_EQUAL(0, ProcessMonitor::createSocketPair(fds));

    // Send token in two chunk fragments
    ssize_t w1 = write(fds[1], "SHUT", 4);
    CHECK_EQUAL(4, w1);
    ssize_t w2 = write(fds[1], "DOWN\n", 5);
    CHECK_EQUAL(5, w2);

    std::string token;
    bool readOk = ProcessMonitor::readIpcToken(fds[0], token);
    CHECK_TRUE(readOk);
    CHECK_EQUAL("SHUTDOWN\n", token);

    close(fds[0]);
    close(fds[1]);
}

TEST(ProcessMonitor_Integrated, ClosedSocketPollHandling) {
    int fds[2] = { -1, -1 };
    CHECK_EQUAL(0, ProcessMonitor::createSocketPair(fds));

    // Close write end immediately
    close(fds[1]);

    struct pollfd pfd;
    pfd.fd = fds[0];
    pfd.events = POLLIN | POLLHUP | POLLERR;
    pfd.revents = 0;

    int pollRet = poll(&pfd, 1, 100);
    CHECK_TRUE(pollRet >= 1);
    CHECK_TRUE((pfd.revents & (POLLHUP | POLLIN)) != 0);

    close(fds[0]);
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
