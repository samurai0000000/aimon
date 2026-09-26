/*
 * ProcessMonitor.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_PROCESS_MONITOR_HXX
#define AIMON_PROCESS_MONITOR_HXX

#include <string>
#include <vector>
#include <cstdint>
#include <sys/types.h>
#include "ConfigManager.hxx"

namespace aimon {

enum class MonitorAction {
    CONTINUE_RUNNING,
    RESPAWN_IMMEDIATE,
    TRIP_CIRCUIT_BREAKER,
    CLEAN_TERMINATION
};

struct CrashIncident {
    int64_t timestampEpoch = 0;
    int exitStatus = 0;
    int termSignal = 0;
    bool coreDumped = false;
    std::string actionTaken;
};

class ProcessMonitor {
public:
    ProcessMonitor(const SupervisorConfig& supervisorConfig,
                   const GeminiConfig& geminiConfig,
                   int argc,
                   char* argv[]);
    ~ProcessMonitor();

    int run();

    static void handleParentSignal(int signum);
    static void setChildIpcFd(int fd);
    static int getChildIpcFd();
    static void notifyWorkerShutdown();

    pid_t getChildPid() const { return _childPid; }
    int getParentIpcFd() const { return _ipcSocketFds[0]; }
    bool checkCleanShutdownReceived() const { return _cleanShutdownReceived; }
    const std::vector<CrashIncident>& getIncidentHistory() const { return _incidentHistory; }

    MonitorAction evaluateChildExit(int status);
    pid_t spawnChild(bool safeMode = false, bool usePrevBinary = false);
    void forwardSignalToChild(int signum);

    // Circuit breaker & quarantine methods
    bool executeDbQuarantine(const std::string& customBaseDir = "");
    bool executeRollback();
    int calculateBackoffSec() const;
    void pruneCrashTimestamps(int64_t nowEpoch);
    void resetCrashHistory();

    bool isSafeModeActive() const { return _safeModeActive; }
    void setSafeModeActive(bool active) { _safeModeActive = active; }
    bool isRollbackActive() const { return _rollbackActive; }
    void setRollbackActive(bool active) { _rollbackActive = active; }
    int getConsecutiveCrashes() const { return _consecutiveCrashes; }
    size_t getActiveCrashCount() const { return _crashTimestamps.size(); }
    void setChildLaunchEpoch(int64_t epoch) { _childLaunchEpoch = epoch; }

    // Socketpair & IPC utilities
    static int createSocketPair(int fds[2]);
    static bool sendIpcToken(int fd, const std::string& token);
    static bool readIpcToken(int fd, std::string& outToken);

private:
    void setupParentSignals();
    void closeParentFds();

    SupervisorConfig _supervisorConfig;
    GeminiConfig _geminiConfig;
    int _argc;
    char** _argv;

    pid_t _childPid = -1;
    int _ipcSocketFds[2] = { -1, -1 };
    std::vector<int64_t> _crashTimestamps;
    std::vector<CrashIncident> _incidentHistory;
    int64_t _childLaunchEpoch = 0;
    int _consecutiveCrashes = 0;
    bool _safeModeActive = false;
    bool _rollbackActive = false;
    bool _cleanShutdownReceived = false;
};

} // namespace aimon

#endif // AIMON_PROCESS_MONITOR_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
