/*
 * ServiceSupervisor.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <array>

#include "ServiceSupervisor.hxx"

namespace aimon {

std::string serviceStateToString(ServiceState state) {
    switch (state) {
        case ServiceState::HEALTHY:
            return "HEALTHY";
        case ServiceState::DEGRADED:
            return "DEGRADED";
        case ServiceState::RESTARTING:
            return "RESTARTING";
        case ServiceState::CRASH_LOOP:
            return "CRASH_LOOP";
        case ServiceState::DISABLED:
            return "DISABLED";
        default:
            return "UNKNOWN";
    }
}

nlohmann::json ServiceRuntimeStatus::toJson() const {
    nlohmann::json j;
    j["id"] = config.id;
    j["name"] = config.name;
    j["host"] = config.host;
    j["port"] = config.port;
    j["secondary_port"] = config.secondaryPort;
    j["enabled"] = config.enabled;
    j["state"] = serviceStateToString(state);
    j["last_probe_epoch"] = lastProbeEpoch;
    j["last_healthy_epoch"] = lastHealthyEpoch;
    j["last_restart_epoch"] = lastRestartEpoch;
    j["probe_latency_ms"] = probeLatencyMs;
    j["consecutive_failures"] = consecutiveFailures;
    j["restart_count"] = restartCount;
    j["recent_restart_timestamps"] = recentRestartTimestamps;
    j["last_error_message"] = lastErrorMessage;
    j["trailing_logs"] = trailingLogs;
    return j;
}

ServiceSupervisor::ServiceSupervisor(const SupervisorConfig& config,
                                     const std::vector<SupervisedServiceConfig>& services,
                                     StateChangedCallback onStateChanged)
    : _config(config),
      _onStateChanged(std::move(onStateChanged)) {
    for (const auto& svc : services) {
        ServiceRuntimeStatus status;
        status.config = svc;
        status.state = svc.enabled ? ServiceState::HEALTHY : ServiceState::DISABLED;
        _services[svc.id] = status;
    }
}

ServiceSupervisor::~ServiceSupervisor() {
    stop();
}

bool ServiceSupervisor::start() {
    if (_running.load()) {
        return false;
    }
    if (!_config.enabled) {
        return false;
    }

    _running.store(true);
    _workerThread = std::make_unique<std::thread>(&ServiceSupervisor::supervisorLoop, this);
    return true;
}

void ServiceSupervisor::stop() {
    if (!_running.load()) {
        return;
    }

    _running.store(false);
    if (_workerThread && _workerThread->joinable()) {
        _workerThread->join();
    }
    _workerThread.reset();
}

std::vector<ServiceRuntimeStatus> ServiceSupervisor::getAllStatuses() const {
    std::shared_lock<std::shared_mutex> lock(_mutex);
    std::vector<ServiceRuntimeStatus> result;
    result.reserve(_services.size());
    for (const auto& kv : _services) {
        result.push_back(kv.second);
    }
    return result;
}

bool ServiceSupervisor::getStatus(const std::string& serviceId,
                                  ServiceRuntimeStatus& outStatus) const {
    std::shared_lock<std::shared_mutex> lock(_mutex);
    auto it = _services.find(serviceId);
    if (it == _services.end()) {
        return false;
    }
    outStatus = it->second;
    return true;
}

void ServiceSupervisor::setStateChangedCallback(StateChangedCallback cb) {
    std::unique_lock<std::shared_mutex> lock(_mutex);
    _onStateChanged = std::move(cb);
}

bool ServiceSupervisor::requestRestart(const std::string& serviceId, bool force) {
    SupervisedServiceConfig cfgToStart;
    StateChangedCallback cb;
    ServiceState oldState = ServiceState::HEALTHY;
    ServiceState newState = ServiceState::RESTARTING;
    bool shouldNotify = false;

    {
        std::unique_lock<std::shared_mutex> lock(_mutex);
        auto it = _services.find(serviceId);
        if (it == _services.end()) {
            return false;
        }

        int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        int windowSec = (_config.crashLoopWindowSec > 0) ? _config.crashLoopWindowSec : 60;
        int maxRetries = (_config.crashLoopMaxRetries > 0) ? _config.crashLoopMaxRetries : 3;

        std::vector<int64_t> fresh;
        for (auto ts : it->second.recentRestartTimestamps) {
            if (now - ts <= windowSec) {
                fresh.push_back(ts);
            }
        }
        it->second.recentRestartTimestamps = fresh;

        if (!force && (int)it->second.recentRestartTimestamps.size() >= maxRetries) {
            oldState = it->second.state;
            it->second.state = ServiceState::CRASH_LOOP;
            newState = it->second.state;
            shouldNotify = (oldState != newState);
            cb = _onStateChanged;
            if (shouldNotify && cb) {
                lock.unlock();
                cb(serviceId, oldState, newState);
            }
            return false;
        }

        it->second.recentRestartTimestamps.push_back(now);
        it->second.restartCount++;
        it->second.lastRestartEpoch = now;

        oldState = it->second.state;
        it->second.state = ServiceState::RESTARTING;
        newState = it->second.state;
        shouldNotify = (oldState != newState);
        cb = _onStateChanged;
        cfgToStart = it->second.config;
    }

    if (shouldNotify && cb) {
        cb(serviceId, oldState, newState);
    }

    return executeRemoteStart(cfgToStart);
}

std::string ServiceSupervisor::getServiceLogs(const std::string& serviceId, int lines) {
    SupervisedServiceConfig cfg;
    {
        std::shared_lock<std::shared_mutex> lock(_mutex);
        auto it = _services.find(serviceId);
        if (it == _services.end()) {
            return "";
        }
        cfg = it->second.config;
    }

    return fetchRemoteLogs(cfg, lines);
}

bool ServiceSupervisor::probeTcpPort(const std::string& host,
                                     int port,
                                     int timeoutMs,
                                     int& outLatencyMs,
                                     std::string& outError) {
    outLatencyMs = 0;
    outError.clear();

    if (port <= 0 || port > 65535) {
        outError = "Invalid port: " + std::to_string(port);
        return false;
    }

    int timeout = (timeoutMs > 0) ? timeoutMs : 1000;

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res = nullptr;
    std::string portStr = std::to_string(port);
    int rc = getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res);
    if (rc != 0 || !res) {
        outError = "DNS resolution failed for " + host + ": " + std::string(gai_strerror(rc));
        return false;
    }

    int fd = socket(res->ai_family, res->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, res->ai_protocol);
    if (fd < 0) {
        outError = "socket() failed: " + std::string(strerror(errno));
        freeaddrinfo(res);
        return false;
    }

    auto start = std::chrono::steady_clock::now();
    int connRc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (connRc == 0) {
        auto end = std::chrono::steady_clock::now();
        outLatencyMs = std::max(1, (int)std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
        close(fd);
        return true;
    }

    if (errno != EINPROGRESS) {
        outError = "connect() failed: " + std::string(strerror(errno));
        close(fd);
        return false;
    }

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLOUT;
    pfd.revents = 0;

    int pollRc = poll(&pfd, 1, timeout);
    if (pollRc == 0) {
        outError = "Connection timed out after " + std::to_string(timeout) + "ms";
        close(fd);
        return false;
    } else if (pollRc < 0) {
        outError = "poll() error: " + std::string(strerror(errno));
        close(fd);
        return false;
    }

    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        int soError = 0;
        socklen_t len = sizeof(soError);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &len);
        outError = "Connection failed: " + std::string(strerror(soError ? soError : ECONNREFUSED));
        close(fd);
        return false;
    }

    if (pfd.revents & POLLOUT) {
        int soError = 0;
        socklen_t len = sizeof(soError);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &len) < 0 || soError != 0) {
            outError = "Connection failed: " + std::string(strerror(soError ? soError : ECONNREFUSED));
            close(fd);
            return false;
        }
        auto end = std::chrono::steady_clock::now();
        outLatencyMs = std::max(1, (int)std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
        close(fd);
        return true;
    }

    outError = "Unexpected socket poll event";
    close(fd);
    return false;
}

void ServiceSupervisor::probeService(ServiceRuntimeStatus& service) {
    if (!service.config.enabled) {
        service.state = ServiceState::DISABLED;
        return;
    }

    int latency = 0;
    std::string err;
    int timeout = (_config.probeTimeoutMs > 0) ? _config.probeTimeoutMs : 1000;
    bool success = probeTcpPort(service.config.host, service.config.port, timeout, latency, err);

    if (!success && service.config.secondaryPort > 0) {
        int secLatency = 0;
        std::string secErr;
        bool secSuccess = probeTcpPort(service.config.host, service.config.secondaryPort, timeout, secLatency, secErr);
        if (secSuccess) {
            success = true;
            latency = secLatency;
            err = "Primary port " + std::to_string(service.config.port) +
                  " unreachable; secondary port " + std::to_string(service.config.secondaryPort) +
                  " responding";
        }
    }

    evaluateStateTransition(service, success, latency, err);
}

void ServiceSupervisor::evaluateStateTransition(ServiceRuntimeStatus& service,
                                                bool probeSuccess,
                                                int latencyMs,
                                                const std::string& errorMsg) {
    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    service.lastProbeEpoch = now;
    service.probeLatencyMs = latencyMs;
    service.lastErrorMessage = errorMsg;

    ServiceState oldState = service.state;
    bool triggerAutoRestart = false;

    if (probeSuccess) {
        service.consecutiveFailures = 0;
        service.lastHealthyEpoch = now;
        service.state = ServiceState::HEALTHY;
    } else {
        service.consecutiveFailures++;
        if (service.consecutiveFailures == 1) {
            service.state = ServiceState::DEGRADED;
        } else if (service.consecutiveFailures >= 2) {
            int windowSec = (_config.crashLoopWindowSec > 0) ? _config.crashLoopWindowSec : 60;
            int maxRetries = (_config.crashLoopMaxRetries > 0) ? _config.crashLoopMaxRetries : 3;

            std::vector<int64_t> fresh;
            for (auto ts : service.recentRestartTimestamps) {
                if (now - ts <= windowSec) {
                    fresh.push_back(ts);
                }
            }
            service.recentRestartTimestamps = fresh;

            if ((int)service.recentRestartTimestamps.size() >= maxRetries) {
                service.state = ServiceState::CRASH_LOOP;
            } else {
                service.state = ServiceState::RESTARTING;
                if (_config.autoRestart) {
                    service.recentRestartTimestamps.push_back(now);
                    service.restartCount++;
                    service.lastRestartEpoch = now;
                    triggerAutoRestart = true;
                }
            }
        }
    }

    if (oldState != service.state && _onStateChanged) {
        _onStateChanged(service.config.id, oldState, service.state);
    }

    if (triggerAutoRestart) {
        executeRemoteStart(service.config);
    }
}

bool ServiceSupervisor::executeRemoteStart(const SupervisedServiceConfig& cfg) {
    if (cfg.startCmd.empty()) {
        return false;
    }

    pid_t pid = fork();
    if (pid == 0) {
        // Child worker process: detach session
        setsid();

        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) {
                close(devnull);
            }
        }

        // Close file descriptors above STDERR
        for (int fd = 3; fd < 256; ++fd) {
            close(fd);
        }

        execlp("/bin/sh", "sh", "-c", cfg.startCmd.c_str(), (char*)NULL);
        _exit(127);
    } else if (pid > 0) {
        return true;
    }

    return false;
}

bool ServiceSupervisor::executeRemoteStop(const SupervisedServiceConfig& cfg) {
    if (cfg.stopCmd.empty()) {
        return false;
    }

    int rc = std::system(cfg.stopCmd.c_str());
    return (rc == 0);
}

std::string ServiceSupervisor::fetchRemoteLogs(const SupervisedServiceConfig& cfg, int lines) {
    if (cfg.logFile.empty()) {
        return "";
    }

    int count = (lines > 0) ? lines : 50;
    std::string cmd;

    if (cfg.host.empty() || cfg.host == "127.0.0.1" || cfg.host == "localhost") {
        cmd = "tail -n " + std::to_string(count) + " " + cfg.logFile + " 2>/dev/null";
    } else {
        cmd = "ssh -n -o BatchMode=yes -o ConnectTimeout=5 " + cfg.host +
              " 'tail -n " + std::to_string(count) + " " + cfg.logFile + "' 2>/dev/null";
    }

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return "";
    }

    std::string result;
    std::array<char, 512> buffer;
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result.append(buffer.data());
    }
    pclose(pipe);

    return result;
}

void ServiceSupervisor::supervisorLoop() {
    while (_running.load()) {
        // Non-blocking reap of any finished child restart processes
        while (waitpid(-1, nullptr, WNOHANG) > 0) {
        }

        std::vector<SupervisedServiceConfig> serviceConfigs;
        {
            std::shared_lock<std::shared_mutex> lock(_mutex);
            serviceConfigs.reserve(_services.size());
            for (const auto& kv : _services) {
                serviceConfigs.push_back(kv.second.config);
            }
        }

        for (const auto& cfg : serviceConfigs) {
            if (!_running.load()) {
                break;
            }

            if (!cfg.enabled) {
                std::unique_lock<std::shared_mutex> lock(_mutex);
                auto it = _services.find(cfg.id);
                if (it != _services.end()) {
                    it->second.state = ServiceState::DISABLED;
                }
                continue;
            }

            int latency = 0;
            std::string err;
            int timeout = (_config.probeTimeoutMs > 0) ? _config.probeTimeoutMs : 1000;
            bool success = probeTcpPort(cfg.host, cfg.port, timeout, latency, err);

            if (!success && cfg.secondaryPort > 0) {
                int secLatency = 0;
                std::string secErr;
                bool secSuccess = probeTcpPort(cfg.host, cfg.secondaryPort, timeout, secLatency, secErr);
                if (secSuccess) {
                    success = true;
                    latency = secLatency;
                    err = "Primary port " + std::to_string(cfg.port) +
                          " unreachable; secondary port " + std::to_string(cfg.secondaryPort) +
                          " responding";
                }
            }

            SupervisedServiceConfig restartCfg;
            bool doRestart = false;

            {
                std::unique_lock<std::shared_mutex> lock(_mutex);
                auto it = _services.find(cfg.id);
                if (it != _services.end()) {
                    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();

                    it->second.lastProbeEpoch = now;
                    it->second.probeLatencyMs = latency;
                    it->second.lastErrorMessage = err;

                    ServiceState oldState = it->second.state;

                    if (success) {
                        it->second.consecutiveFailures = 0;
                        it->second.lastHealthyEpoch = now;
                        it->second.state = ServiceState::HEALTHY;
                    } else {
                        it->second.consecutiveFailures++;
                        if (it->second.consecutiveFailures == 1) {
                            it->second.state = ServiceState::DEGRADED;
                        } else if (it->second.consecutiveFailures >= 2) {
                            int windowSec = (_config.crashLoopWindowSec > 0) ? _config.crashLoopWindowSec : 60;
                            int maxRetries = (_config.crashLoopMaxRetries > 0) ? _config.crashLoopMaxRetries : 3;

                            std::vector<int64_t> fresh;
                            for (auto ts : it->second.recentRestartTimestamps) {
                                if (now - ts <= windowSec) {
                                    fresh.push_back(ts);
                                }
                            }
                            it->second.recentRestartTimestamps = fresh;

                            if ((int)it->second.recentRestartTimestamps.size() >= maxRetries) {
                                it->second.state = ServiceState::CRASH_LOOP;
                            } else {
                                it->second.state = ServiceState::RESTARTING;
                                if (_config.autoRestart) {
                                    it->second.recentRestartTimestamps.push_back(now);
                                    it->second.restartCount++;
                                    it->second.lastRestartEpoch = now;
                                    doRestart = true;
                                    restartCfg = it->second.config;
                                }
                            }
                        }
                    }

                    if (oldState != it->second.state && _onStateChanged) {
                        ServiceState newState = it->second.state;
                        auto cb = _onStateChanged;
                        lock.unlock();
                        cb(cfg.id, oldState, newState);
                    }
                }
            }

            if (doRestart) {
                executeRemoteStart(restartCfg);
            }
        }

        // Sleep in 100ms intervals up to probeIntervalSec
        int intervalSec = (_config.probeIntervalSec > 0) ? _config.probeIntervalSec : 10;
        int intervalMs = intervalSec * 1000;
        int elapsedMs = 0;
        while (_running.load() && elapsedMs < intervalMs) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            elapsedMs += 100;
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
