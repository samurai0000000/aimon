/*
 * ServiceSupervisor.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_SERVICE_SUPERVISOR_HXX
#define AIMON_SERVICE_SUPERVISOR_HXX

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <functional>
#include <cstdint>
#include <nlohmann/json.hpp>
#include "ConfigManager.hxx"

namespace aimon {

enum class ServiceState {
    HEALTHY,
    DEGRADED,
    RESTARTING,
    CRASH_LOOP,
    DISABLED
};

std::string serviceStateToString(ServiceState state);

struct ServiceRuntimeStatus {
    SupervisedServiceConfig config;
    ServiceState state = ServiceState::HEALTHY;
    int64_t lastProbeEpoch = 0;
    int64_t lastHealthyEpoch = 0;
    int64_t lastRestartEpoch = 0;
    int probeLatencyMs = 0;
    int consecutiveFailures = 0;
    int restartCount = 0;
    std::vector<int64_t> recentRestartTimestamps;
    std::string lastErrorMessage = "";
    std::string trailingLogs = "";

    nlohmann::json toJson() const;
};

class ServiceSupervisor {
public:
    using StateChangedCallback = std::function<void(const std::string& serviceId,
                                                    ServiceState oldState,
                                                    ServiceState newState)>;

    ServiceSupervisor(const SupervisorConfig& config,
                      const std::vector<SupervisedServiceConfig>& services,
                      StateChangedCallback onStateChanged = nullptr);
    ~ServiceSupervisor();

    bool start();
    void stop();
    bool isRunning() const { return _running.load(); }

    std::vector<ServiceRuntimeStatus> getAllStatuses() const;
    bool getStatus(const std::string& serviceId, ServiceRuntimeStatus& outStatus) const;
    bool requestRestart(const std::string& serviceId, bool force = false);
    std::string getServiceLogs(const std::string& serviceId, int lines = 50);

    void setStateChangedCallback(StateChangedCallback cb);

    // Static helper for TCP port probe (loopback/network with non-blocking poll)
    static bool probeTcpPort(const std::string& host,
                             int port,
                             int timeoutMs,
                             int& outLatencyMs,
                             std::string& outError);

    // Internal probing and state evaluation
    void probeService(ServiceRuntimeStatus& service);
    void evaluateStateTransition(ServiceRuntimeStatus& service,
                                 bool probeSuccess,
                                 int latencyMs,
                                 const std::string& errorMsg);

    bool executeRemoteStart(const SupervisedServiceConfig& cfg);
    bool executeRemoteStop(const SupervisedServiceConfig& cfg);
    std::string fetchRemoteLogs(const SupervisedServiceConfig& cfg, int lines);

private:
    void supervisorLoop();

    SupervisorConfig _config;
    StateChangedCallback _onStateChanged;

    std::atomic<bool> _running{false};
    std::unique_ptr<std::thread> _workerThread;
    mutable std::shared_mutex _mutex;
    std::map<std::string, ServiceRuntimeStatus> _services;
};

} // namespace aimon

#endif // AIMON_SERVICE_SUPERVISOR_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
