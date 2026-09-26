/*
 * ProcessMonitor.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "ProcessMonitor.hxx"
#include "PathUtils.hxx"
#include <iostream>
#include <vector>
#include <string>
#include <atomic>
#include <cstring>
#include <ctime>
#include <csignal>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

namespace aimon {

static std::atomic<int> s_caughtSignal(0);
static std::atomic<int> s_childIpcFd(-1);

void ProcessMonitor::handleParentSignal(int signum) {
    s_caughtSignal.store(signum);
}

void ProcessMonitor::setChildIpcFd(int fd) {
    s_childIpcFd.store(fd);
}

int ProcessMonitor::getChildIpcFd() {
    return s_childIpcFd.load();
}

void ProcessMonitor::notifyWorkerShutdown() {
    int fd = s_childIpcFd.load();
    if (fd >= 0) {
        const char* msg = "SHUTDOWN\n";
        ssize_t ret = ::write(fd, msg, 9);
        (void)ret;
    }
}

int ProcessMonitor::createSocketPair(int fds[2]) {
    int ret = socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds);
    if (ret < 0) {
        return ret;
    }
    for (int i = 0; i < 2; ++i) {
        int flags = fcntl(fds[i], F_GETFL, 0);
        if (flags >= 0) {
            fcntl(fds[i], F_SETFL, flags | O_NONBLOCK);
        }
    }
    return 0;
}

bool ProcessMonitor::sendIpcToken(int fd, const std::string& token) {
    if (fd < 0) return false;
    std::string toSend = token;
    if (toSend.empty() || toSend.back() != '\n') {
        toSend += '\n';
    }
    ssize_t written = ::write(fd, toSend.data(), toSend.size());
    return written == static_cast<ssize_t>(toSend.size());
}

bool ProcessMonitor::readIpcToken(int fd, std::string& outToken) {
    if (fd < 0) return false;
    char buf[256];
    ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        outToken.assign(buf, n);
        return true;
    }
    return false;
}

ProcessMonitor::ProcessMonitor(const SupervisorConfig& supervisorConfig,
                               const GeminiConfig& geminiConfig,
                               int argc,
                               char* argv[])
    : _supervisorConfig(supervisorConfig),
      _geminiConfig(geminiConfig),
      _argc(argc),
      _argv(argv) {
}

ProcessMonitor::~ProcessMonitor() {
    closeParentFds();
}

void ProcessMonitor::closeParentFds() {
    for (int i = 0; i < 2; ++i) {
        if (_ipcSocketFds[i] >= 0) {
            close(_ipcSocketFds[i]);
            _ipcSocketFds[i] = -1;
        }
    }
}

void ProcessMonitor::setupParentSignals() {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = ProcessMonitor::handleParentSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);
    sigaction(SIGCHLD, &sa, nullptr);
}

void ProcessMonitor::forwardSignalToChild(int signum) {
    if (_childPid > 0) {
        ::kill(_childPid, signum);
    }
}

pid_t ProcessMonitor::spawnChild(bool safeMode, bool usePrevBinary) {
    if (createSocketPair(_ipcSocketFds) < 0) {
        perror("[ProcessMonitor] socketpair failed");
        return -1;
    }

    // Prepare arguments
    std::vector<std::string> args;
    std::string prog = _argv[0];
    if (usePrevBinary && !_supervisorConfig.prevBinaryPath.empty()) {
        prog = _supervisorConfig.prevBinaryPath;
    }

    args.push_back(prog);
    for (int i = 1; i < _argc; ++i) {
        std::string a = _argv[i];
        if (a == "--child-worker" || a == "--no-supervisor" || a.rfind("--ipc-fd=", 0) == 0) {
            continue;
        }
        if (a == "--ipc-fd" && i + 1 < _argc) {
            ++i;
            continue;
        }
        args.push_back(a);
    }

    args.push_back("--child-worker");
    args.push_back("--ipc-fd=" + std::to_string(_ipcSocketFds[1]));
    if (safeMode) {
        args.push_back("--safe-mode");
    }

    std::vector<char*> cArgs;
    for (auto& s : args) {
        cArgs.push_back(s.data());
    }
    cArgs.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) {
        perror("[ProcessMonitor] fork failed");
        closeParentFds();
        return -1;
    }

    if (pid == 0) {
        // Child worker
        close(_ipcSocketFds[0]);
        _ipcSocketFds[0] = -1;

        // Clear CLOEXEC on child IPC fd so execvp preserves it
        fcntl(_ipcSocketFds[1], F_SETFD, 0);

        execvp(prog.c_str(), cArgs.data());
        perror("[ProcessMonitor] execvp failed");
        _exit(127);
    }

    // Parent monitor
    close(_ipcSocketFds[1]);
    _ipcSocketFds[1] = -1;
    _childPid = pid;
    _childLaunchEpoch = time(nullptr);
    _cleanShutdownReceived = false;

    return pid;
}

void ProcessMonitor::pruneCrashTimestamps(int64_t nowEpoch) {
    int64_t cutoff = nowEpoch - _supervisorConfig.crashLoopWindowSec;
    _crashTimestamps.erase(
        std::remove_if(_crashTimestamps.begin(), _crashTimestamps.end(),
                       [cutoff](int64_t ts) { return ts < cutoff; }),
        _crashTimestamps.end()
    );
}

void ProcessMonitor::resetCrashHistory() {
    _crashTimestamps.clear();
    _consecutiveCrashes = 0;
    _safeModeActive = false;
    _rollbackActive = false;
}

int ProcessMonitor::calculateBackoffSec() const {
    if (_consecutiveCrashes <= 0) return 0;
    int shift = std::min(_consecutiveCrashes - 1, 6);
    int backoff = _supervisorConfig.backoffInitialSec * (1 << shift);
    return std::min(backoff, _supervisorConfig.backoffMaxSec);
}

bool ProcessMonitor::executeDbQuarantine(const std::string& customBaseDir) {
    std::string baseDir = customBaseDir.empty() ? PathUtils::getAimonConfigDir() : PathUtils::expandHome(customBaseDir);
    if (!fs::exists(baseDir)) return false;

    int64_t now = time(nullptr);
    std::string quarantineDir = baseDir + "/quarantine/" + std::to_string(now);
    try {
        fs::create_directories(quarantineDir);
    } catch (...) {
        return false;
    }

    bool movedAny = false;
    try {
        for (const auto& entry : fs::directory_iterator(baseDir)) {
            if (entry.is_regular_file()) {
                std::string fname = entry.path().filename().string();
                if (fname.find(".db") != std::string::npos) {
                    std::string target = quarantineDir + "/" + fname;
                    fs::rename(entry.path(), target);
                    movedAny = true;
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[ProcessMonitor] DB quarantine move error: " << e.what() << std::endl;
    }

    try {
        std::string qParent = baseDir + "/quarantine";
        if (fs::exists(qParent)) {
            std::vector<fs::directory_entry> qDirs;
            for (const auto& entry : fs::directory_iterator(qParent)) {
                if (entry.is_directory()) {
                    qDirs.push_back(entry);
                }
            }
            if (qDirs.size() > static_cast<size_t>(_supervisorConfig.maxDbQuarantineVersions)) {
                std::sort(qDirs.begin(), qDirs.end(), [](const auto& a, const auto& b) {
                    return a.path().filename().string() < b.path().filename().string();
                });
                size_t toDelete = qDirs.size() - _supervisorConfig.maxDbQuarantineVersions;
                for (size_t i = 0; i < toDelete; ++i) {
                    fs::remove_all(qDirs[i].path());
                }
            }
        }
    } catch (...) {}

    return movedAny;
}

bool ProcessMonitor::executeRollback() {
    std::string prev = PathUtils::expandHome(_supervisorConfig.prevBinaryPath);
    if (!prev.empty() && fs::exists(prev) && access(prev.c_str(), X_OK) == 0) {
        _rollbackActive = true;
        return true;
    }
    return false;
}

MonitorAction ProcessMonitor::evaluateChildExit(int status) {
    int exitCode = 0;
    int termSig = 0;
    bool coreDump = false;

    if (WIFEXITED(status)) {
        exitCode = WEXITSTATUS(status);
        if (exitCode == 0) {
            _consecutiveCrashes = 0;
            return MonitorAction::CLEAN_TERMINATION;
        }
    } else if (WIFSIGNALED(status)) {
        termSig = WTERMSIG(status);
#ifdef WCOREDUMP
        coreDump = WCOREDUMP(status);
#endif
    }

    int64_t now = time(nullptr);
    pruneCrashTimestamps(now);

    if (_childLaunchEpoch > 0 && (now - _childLaunchEpoch) >= _supervisorConfig.crashLoopWindowSec) {
        _consecutiveCrashes = 1;
    } else {
        _consecutiveCrashes++;
    }

    _crashTimestamps.push_back(now);

    CrashIncident incident;
    incident.timestampEpoch = now;
    incident.exitStatus = exitCode;
    incident.termSignal = termSig;
    incident.coreDumped = coreDump;

    std::cerr << "[ProcessMonitor] Child PID " << _childPid << " terminated abnormally ("
              << (termSig ? "signal " + std::to_string(termSig) : "exit code " + std::to_string(exitCode))
              << (coreDump ? " (core dumped)" : "") << ", crashes in window: "
              << _crashTimestamps.size() << "/" << _supervisorConfig.crashLoopMaxRetries << ")" << std::endl;

    if (static_cast<int>(_crashTimestamps.size()) >= _supervisorConfig.crashLoopMaxRetries) {
        if (!_safeModeActive) {
            std::cerr << "[ProcessMonitor] Circuit breaker: maximum crash threshold reached. Initiating DB quarantine & safe mode." << std::endl;
            executeDbQuarantine();
            _safeModeActive = true;
            incident.actionTaken = "QUARANTINE_AND_SAFE_MODE";
            _incidentHistory.push_back(incident);
            return MonitorAction::RESPAWN_IMMEDIATE;
        }

        if (!_rollbackActive && executeRollback()) {
            std::cerr << "[ProcessMonitor] Safe mode failed. Rolling back to previous binary: "
                      << _supervisorConfig.prevBinaryPath << std::endl;
            incident.actionTaken = "ROLLBACK_BINARY";
            _incidentHistory.push_back(incident);
            return MonitorAction::RESPAWN_IMMEDIATE;
        }

        std::cerr << "[ProcessMonitor] Circuit breaker TRIPPED! Halting supervision to prevent thrashing." << std::endl;
        incident.actionTaken = "TRIP_CIRCUIT_BREAKER";
        _incidentHistory.push_back(incident);
        return MonitorAction::TRIP_CIRCUIT_BREAKER;
    }

    incident.actionTaken = "RESPAWN";
    _incidentHistory.push_back(incident);
    return MonitorAction::RESPAWN_IMMEDIATE;
}

int ProcessMonitor::run() {
    setupParentSignals();

    if (spawnChild(_safeModeActive, _rollbackActive) < 0) {
        std::cerr << "[ProcessMonitor] Failed to spawn initial child worker." << std::endl;
        return 1;
    }

    std::cout << "[ProcessMonitor] Started child worker PID " << _childPid << std::endl;

    while (true) {
        int sig = s_caughtSignal.exchange(0);
        if (sig == SIGINT || sig == SIGTERM || sig == SIGHUP) {
            std::cout << "[ProcessMonitor] Forwarding signal " << sig << " to child PID " << _childPid << std::endl;
            forwardSignalToChild(sig);
        }

        struct pollfd pfd;
        pfd.fd = _ipcSocketFds[0];
        pfd.events = POLLIN | POLLHUP | POLLERR;
        pfd.revents = 0;

        int pollRet = poll(&pfd, 1, 200);
        if (pollRet > 0) {
            if (pfd.revents & POLLIN) {
                char buf[256];
                ssize_t bytesRead = ::read(_ipcSocketFds[0], buf, sizeof(buf) - 1);
                if (bytesRead > 0) {
                    buf[bytesRead] = '\0';
                    std::string msg(buf);
                    if (msg.find("SHUTDOWN") != std::string::npos) {
                        _cleanShutdownReceived = true;
                    }
                }
            }
        }

        int status = 0;
        pid_t reaped = waitpid(_childPid, &status, WNOHANG);
        if (reaped == _childPid) {
            MonitorAction action = evaluateChildExit(status);
            if (action == MonitorAction::CLEAN_TERMINATION) {
                std::cout << "[ProcessMonitor] Child worker exited cleanly (code 0). Terminating monitor." << std::endl;
                closeParentFds();
                return 0;
            } else if (action == MonitorAction::RESPAWN_IMMEDIATE) {
                closeParentFds();
                int backoff = calculateBackoffSec();
                if (backoff > 0) {
                    std::cout << "[ProcessMonitor] Backoff delay: " << backoff << "s before respawn..." << std::endl;
                    sleep(backoff);
                }
                if (spawnChild(_safeModeActive, _rollbackActive) < 0) {
                    std::cerr << "[ProcessMonitor] Failed to respawn child worker." << std::endl;
                    return 1;
                }
                std::cout << "[ProcessMonitor] Respawned child worker PID " << _childPid << std::endl;
            } else if (action == MonitorAction::TRIP_CIRCUIT_BREAKER) {
                std::cerr << "[ProcessMonitor] Circuit breaker tripped. Stopping supervision." << std::endl;
                closeParentFds();
                return 1;
            }
        } else if (reaped < 0 && errno == ECHILD) {
            closeParentFds();
            return _cleanShutdownReceived ? 0 : 1;
        }
    }
}

} // namespace aimon

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
