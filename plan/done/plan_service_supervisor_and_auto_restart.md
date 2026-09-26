# Plan: Multi-Host Service Supervisor & Automated Process Recovery

- **Date**: 2026-09-26
- **Target Platform / Scope**: Personal developer infrastructure (`builder`, `fox`, `rhino`), central daemon (`aimon`), and satellite daemons (`meshmon`, `netmon`, `embdevenv`).
- **Status**: Done
- **Lifecycle Location**: `plan/done/`
- **Artifacts Directory**: `plan/done/plan_service_supervisor_and_auto_restart.artifacts/`
- **Agent Mode**: Single-Agent Pair Programming
- **Core Objectives**:
  1. **Dedicated Forked Monitor (`aimon-monitor`)**: Supervise the central `aimon` daemon via a dedicated, lightweight parent monitor process watching *only* `aimon`, with strict clean exit symmetry on graceful shutdown.
  2. **Fleet Service Supervision**: Enable `aimon` to monitor all remote and local satellite daemons (`meshmon`, `netmon`, `embdevenv`) via non-blocking TCP socket probes, automatic state tracking, and asynchronous background restart execution over SSH.
  3. **Anti-Thrashing Circuit Breaker & Staged Recovery**: Prevent infinite crash loops through sliding-window failure tracking, automated SQLite database quarantine, safe-mode execution fallback, and binary rollback.
  4. **Zero IDE & Zero Screen Dependency**: 100% headless, server-native POSIX background daemonization with PID files and file logging. Strictly no GNU `screen`, `tmux`, terminal scraping, or IDE language servers.
  5. **Headless, Optional Gemini Cloud Triage**: Implement direct cloud HTTPS diagnostic client to Google Gemini REST API (`generativelanguage.googleapis.com`) using `GEMINI_API_KEY`. Strictly disabled by default (`gemini.enabled = false;`).
  6. **Complete Configuration via `libconfig++`**: Declare and parse all supervisor, service, recovery, and AI options strictly via `libconfig++` (`~/.config/aimon/aimon.cfg`).
  7. **Strict Test Qualification Gates**: Every implementation envelope must be qualified by a dedicated automated test suite. If tests do not pass 100%, implementation cannot advance to the next envelope under any circumstances.

---

## 0. Corrections & Negative Constraints (Do Not Reintroduce)

1. **No GNU Screen / Tmux Automation**: Do not use `screen -S`, `screen -X stuff`, `hardcopy` scraping, or simulated terminal keystrokes to control daemons. All services operate as true background POSIX daemons with PID files and redirected file logging (`~/.config/<daemon>/<daemon>.log`).
2. **No IDE-Bound Language Server or AgentAPI**: Do not rely on Antigravity IDE, Cursor, VS Code, or IDE-spawned language servers (`language_server_linux_x64`, `agentapi`). The monitoring and recovery engine must function autonomously on headless servers where IDEs are completely absent.
3. **No Single-Process Self-Supervision Monolith**: Do not attempt to have `aimon` restart itself after an internal fatal crash (e.g. `SIGSEGV`, `SIGABRT`, unhandled exception). A process cannot resurrect itself once terminated; a separate, dedicated parent monitor process (`aimon-monitor`) must supervise `aimon`.
4. **No Infinite Crash Respawn Loops (Anti-Thrashing)**: Do not implement naive respawn logic that immediately and perpetually restarts a crashing binary. Repeated crashes must trip a circuit breaker and escalate through database quarantine, `--safe-mode`, and binary rollback.
5. **No Mandatory Cloud or AI Blocking Path**: Do not make AI triage a required or blocking step for daemon restart. The Gemini client must remain strictly optional, dormant by default, and isolated from the core restart path.
6. **No Ad-Hoc / Hand-Rolled Configuration Syntax**: Do not invent custom configuration formats or ad-hoc key-value parsers. All configuration parameters must be declared and parsed using the standard `libconfig++` library (consistent with `meshmon.cfg` and `netmon.cfg`).
7. **No Unqualified Envelope Progression**: Do not write code or create files for Envelope $N+1$ until Envelope $N$'s qualification test suite compiles, runs against real OS primitives, and achieves 100% pass verification.
8. **No Third-Party Corporate Entities / Names**: Never reference, infer, or hallucinate third-party corporate or employer names in any file, license block, README, comment, commit message, build script, documentation, or header.
9. **No Hardcoded Absolute User Paths or Leaked Physical MACs**: Never hardcode `/home/<user>/...` (use `$HOME` or `~`), never commit private credentials or SSH keys, and never embed physical device MAC addresses in source code or documentation.
10. **Zero-Mock Policy (Gemini Cloud REST API is the Sole Permitted Mock)**: Never mock local operating system primitives, processes, sockets, databases, daemons, or network connections. All tests across Envelopes 1, 2, 3, 4, 6, and 7 must execute against real physical primitives (real `fork()`, real `socketpair()`, real non-blocking TCP sockets on loopback `127.0.0.1`, real POSIX signals, real SQLite database files and atomic renames, real HTTP server sockets). The external third-party Google Gemini Cloud REST API in Envelope 5 is the sole permitted mock in the entire testing regime (using an offline loopback server to prevent third-party internet dependencies and cloud API quota consumption).

---

## 1. Execution Boundaries & Strict Guardrails

During execution, the assistant operates strictly under these boundaries:

1. **Direct Question Answering (Virtual Ask Mode)**:
   - Answer all user questions and inquiries directly in conversational markdown text using read-only inspection tools.
   - Strictly no unsolicited file edits or scratch file generation during exploratory Q&A.
2. **Mandatory User Authorization Gate (Virtual Plan Mode)**:
   - Never modify source code, configuration files, or documentation without an approved plan and explicit user instruction to proceed.
3. **Plan Artifacts Directory**:
   - All generated mockups, analysis logs, diagrams, and supplementary artifacts must be saved under `plan/plan_service_supervisor_and_auto_restart.artifacts/`.
4. **Anti-Sycophancy & Code Completeness (Gemini Inoculation)**:
   - Provide critical technical friction; do not agree with flawed premises or broken architecture.
   - Strictly no code stubs, `// TODO` comments, or truncated blocks (`/* ... */`). All edits must be 100% complete and drop-in compilable.
5. **Zero-Tolerance Guessing & Speculative Workarounds**:
   - Never guess commands, flags, parameters, paths, hardware addresses, or internal APIs.
   - Stop immediately upon encountering unexpected errors or discrepancies and report raw facts to the user.
6. **Physical Ground Truth & Real Verification (Proper Testing Standard)**:
   - Invalidate proxy-only checks (`systemctl is-active`, `pgrep` presence alone) and tautological tests.
   - Require real end-to-end I/O verification (real POSIX process forks, `socketpair` streams, non-blocking TCP socket connects, real signal handling, and atomic filesystem renames) before declaring completion.
   - **All envelopes must be qualified with proper testing. If tests do not pass, implementation cannot advance to the next envelope under any circumstances.**
   - **Definition of Proper Testing (All Using CppUTest)**:
     1. **Repeatable**: Deterministic, hermetic, fully automated CLI execution (`make test` or test runner invocation) producing identical results across runs with zero flakiness and clean fixture setup/teardown.
     2. **Coverage / Boundary / Fault**: Comprehensive test matrices exercising nominal paths, boundary limits (min/max thresholds, edge cases, buffer/state bounds), and fault injection (simulated errors, invalid inputs, timeouts, disconnects, crash recovery).
     3. **Formalized Across All Testing Scopes in CppUTest**:
        - **Unit Testing**: Component-level isolation tests (`TEST_GROUP(<Component>)`) verifying classes, methods, and algorithmic logic.
        - **Integrated Testing**: Subsystem and cross-module integration tests (`TEST_GROUP(Integration_<Subsystem>)`) verifying live component interactions (e.g. IPC sockets, protocols, event loops, filesystem stores).
        - **Regression Testing**: Accumulative test suite execution (`make test`) verifying that all previously implemented and passed CppUTest test groups continue to pass with 100% assertions and zero memory leaks before advancing past any envelope hardstop.
     4. **Strict Zero-Mock Ground-Truth Invariant**: External third-party Google Gemini Cloud REST API is the sole permitted mock in the entire test suite. Mocking local OS primitives, processes, filesystems, sockets, or daemons is strictly prohibited. Envelopes 1, 2, 3, 4, 6, and 7 must test against physical ground truth (real processes, real socketpairs, real loopback TCP sockets, real filesystem I/O, real signals).
7. **Surgical Edits & Code Integrity**:
   - Use targeted line replacements (`replace_file_content` or `multi_replace_file_content`).
   - Preserve all existing comments, docstrings, license headers, and surrounding code formatting.
8. **Git Commit Prohibition & Pre-Commit Review**:
   - Never execute `git commit` without an explicit, direct command from the user.
   - Run the pre-commit compliance check before any commit is created.
9. **Project-Specific Boundaries & Safety Invariants**:
   - Use top-level `Makefile` wrapping CMake. Never invoke `cmake` directly.
   - Adhere strictly to C++ standards: `.cxx` for source, `.hxx` for headers, PascalCase filenames (`TestTarget.cxx`), BSD style with 4-space indentation, no tabs.

---

## 2. Technical Approach & Architecture

### 2.1 Purpose and Non-Negotiable Acceptance Criteria

This plan freezes the architecture for multi-host daemon supervision, local self-healing, remote process recovery, and optional cloud crash triage in `aimon`.

The design must satisfy all five core requirements:
1. **Clean Exit Symmetry**: If `aimon` is requested to shut down (`Ctrl-C`, `SIGTERM`, `SIGINT`, or clean exit command), `aimon-monitor` catches the clean shutdown token/code and exits normally (exit code `0`) without attempting to restart.
2. **Dedicated Forked Monitor**: `aimon-monitor` supervises *only* `aimon`. `aimon` itself supervises *all other* monitor applications (`meshmon`, `netmon`, `embdevenv`).
3. **Anti-Thrashing Escalation**: A crashing daemon must not spin infinitely. The supervisor implements a 3-strike sliding window tripping a circuit breaker that executes database quarantine, safe-mode execution, and binary rollback.
4. **Zero IDE & Zero Screen Dependency**: 100% headless POSIX background daemonization with PID files and file logging.
5. **Headless, Optional Gemini Cloud Triage**: Direct cloud HTTPS client to Google Gemini REST API using `GEMINI_API_KEY`, strictly dormant by default (`enabled = false;`).

### 2.2 System Architecture Blueprint

```text
+----------------------------------------------------------------------------+
| PHYSICAL HOST: builder (Central Intelligence Node)                         |
|                                                                            |
| +------------------------------------------------------------------------+ |
| | aimon-monitor (Dedicated Forked Parent Process)                        | |
| |                                                                        | |
| | - Watches ONLY aimon child process                                     | |
| | - Anonymous IPC socketpair stream ("SHUTDOWN\n" clean token)           | |
| | - Signal forwarding (SIGTERM / SIGINT / SIGHUP)                        | |
| | - Anti-Thrashing Circuit Breaker (3 crashes / 60s window)              | |
| | - Escalation: DB Quarantine -> --safe-mode -> ./build/aimon.prev       | |
| | - Optional: Headless Gemini Cloud Triage (Dormant by default)          | |
| +------------------------------------------------------------------------+ |
|       | fork() / waitpid()          ^ socketpair IPC / exit status         |
|       v                             |                                      |
| +------------------------------------------------------------------------+ |
| | aimon (Central Daemon & Fleet Supervisor)                              | |
| |                                                                        | |
| | - Web Dashboard & REST API (Port 3883)                                 | |
| | - Gateway TCP Listener (Port 3885)                                     | |
| | - ServiceSupervisor Engine (Watches OTHER applications)                | |
| |     * Non-blocking TCP connect probes                                  | |
| |     * State Machine: HEALTHY -> DEGRADED -> RESTARTING -> CRASH_LOOP   | |
| |     * Asynchronous direct SSH remote start/stop dispatch               | |
| +------------------------------------------------------------------------+ |
+----------------------------------------------------------------------------+
       |                                              |
       | TCP Probes (16880/16876)                     | TCP Probes (3884, 3886)
       | SSH Start/Stop ("ssh -n fox ...")            | SSH Start/Stop ("ssh -n rhino ...")
       v                                              v
+-----------------------------+               +------------------------------+
| SATELLITE HOST: fox         |               | SATELLITE HOST: rhino        |
|                             |               |                              |
| - meshmon (PID 595091)      |               | - netmon (Port 3884)         |
|   Web: 16880, Shell: 16876  |               |   PID: ~/.config/netmon.pid  |
|   PID: ~/.config/meshmon.pid|               | - embdevenv (Port 3886)      |
|   Log: ~/.config/meshmon.log|               |   PID: ~/.config/embdev.pid  |
+-----------------------------+               +------------------------------+
```

### 2.3 Complete Configuration Specification (`~/.config/aimon/aimon.cfg`)

The complete configuration file is located at `~/.config/aimon/aimon.cfg` (or passed via `-c / --config <path>`). Every parameter is parsed strictly via `libconfig++`:

```cfg
# ~/.config/aimon/aimon.cfg
# Aimon Unified Monitor & Multi-Host Supervisor Configuration

polling : 
{
  base_interval_sec = 300;
  idle_interval_sec = 600;
  active_interval_sec = 120;
};

web : 
{
  host = "0.0.0.0";
  port = 3883;
  endpoints_enabled = true;
};

mqtt : 
{
  enabled = false;
  broker = "localhost";
  port = 1883;
  username = "";
  password = "";
  topic_prefix = "aimon";
  discovery_prefix = "homeassistant";
  retain = true;
};

history : 
{
  enabled = true;
  db_path = "~/.config/aimon/history.db";
};

antigravity : 
{
  auto_discover = true;
  port = 0;
  csrf_token = "";
  default_model = "Antigravity";
};

cursor : 
{
  auto_discover = true;
  db_path = "";
  access_token = "";
};

gateway : 
{
  enabled = true;
  host = "0.0.0.0";
  port = 3885;
};

supervisor : 
{
  enabled = true;
  poll_interval_sec = 10;
  probe_timeout_ms = 1500;
  healthy_uptime_threshold_sec = 60;
  crash_loop_max_retries = 3;
  crash_loop_window_sec = 60;
  backoff_initial_sec = 2;
  backoff_max_sec = 30;
  quarantine_enabled = true;
  safe_mode_fallback = true;
  binary_rollback_enabled = true;
};

services = (
  {
    id = "meshmon";
    name = "Mesh Monitor";
    host = "192.168.8.245";
    probe_port = 16880;
    secondary_port = 16876;
    enabled = true;
    start_cmd = "ssh -n fox 'cd ~/work/meshmon && ./build/aarch64/meshmon -b -D ~/.config/meshmon/meshmon.db >> ~/.config/meshmon/meshmon.log 2>&1'";
    stop_cmd = "ssh -n fox 'pkill -SIGTERM -f build/aarch64/meshmon'";
    status_cmd = "ssh -n fox 'pgrep -f build/aarch64/meshmon'";
    pid_file = "~/.config/meshmon/meshmon.pid";
    log_file = "~/.config/meshmon/meshmon.log";
  },
  {
    id = "netmon";
    name = "Network Monitor";
    host = "192.168.8.30";
    probe_port = 3884;
    secondary_port = 0;
    enabled = true;
    start_cmd = "ssh -n rhino 'cd ~/work/netmon && ./build/netmon daemon -b >> ~/.config/netmon/netmon.log 2>&1'";
    stop_cmd = "ssh -n rhino 'pkill -SIGTERM -f build/netmon'";
    status_cmd = "ssh -n rhino 'pgrep -f build/netmon'";
    pid_file = "~/.config/netmon/netmon.pid";
    log_file = "~/.config/netmon/netmon.log";
  },
  {
    id = "embdevenv";
    name = "Embedded Dev";
    host = "192.168.8.30";
    probe_port = 3886;
    secondary_port = 0;
    enabled = true;
    start_cmd = "ssh -n rhino 'cd ~/work/embdevenv && ./build/embdevenv -b >> ~/.config/embdevenv/embdevenv.log 2>&1'";
    stop_cmd = "ssh -n rhino 'pkill -SIGTERM -f build/embdevenv'";
    status_cmd = "ssh -n rhino 'pgrep -f build/embdevenv'";
    pid_file = "~/.config/embdevenv/embdevenv.pid";
    log_file = "~/.config/embdevenv/embdevenv.log";
  }
);

gemini : 
{
  enabled = false;
  api_key = "";
  model = "gemini-2.5-flash";
  timeout_sec = 30;
  auto_triage_on_crash = false;
  max_tokens = 1024;
  temperature = 0.2;
};
```

### 2.4 C++ Data Structures & Interfaces

#### 2.4.1 `include/ConfigManager.hxx`
```cpp
/*
 * ConfigManager.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_CONFIG_MANAGER_HXX
#define AIMON_CONFIG_MANAGER_HXX

#include <string>
#include <vector>
#include <cstdint>
#include <nlohmann/json.hpp>

namespace aimon {

struct PollingConfig {
    int baseIntervalSec = 300;
    int idleIntervalSec = 600;
    int activeIntervalSec = 120;
};

struct WebConfig {
    std::string host = "0.0.0.0";
    int port = 3883;
    bool endpointsEnabled = true;
};

struct MqttConfig {
    bool enabled = false;
    std::string broker = "localhost";
    int port = 1883;
    std::string username = "";
    std::string password = "";
    std::string topicPrefix = "aimon";
    std::string discoveryPrefix = "homeassistant";
    bool retain = true;
};

struct HistoryConfig {
    bool enabled = true;
    std::string dbPath = "";
};

struct AntigravityConfig {
    bool autoDiscover = true;
    int port = 0;
    std::string csrfToken = "";
    std::string defaultModel = "Antigravity";
};

struct CursorConfig {
    bool autoDiscover = true;
    std::string dbPath = "";
    std::string accessToken = "";
};

struct GatewayConfig {
    bool enabled = true;
    std::string host = "0.0.0.0";
    int port = 3885;
};

struct SupervisorConfig {
    bool enabled = true;
    int pollIntervalSec = 10;
    int probeTimeoutMs = 1500;
    int healthyUptimeThresholdSec = 60;
    int crashLoopMaxRetries = 3;
    int crashLoopWindowSec = 60;
    int backoffInitialSec = 2;
    int backoffMaxSec = 30;
    bool quarantineEnabled = true;
    bool safeModeFallback = true;
    bool binaryRollbackEnabled = true;
};

struct SupervisedServiceConfig {
    std::string id = "";
    std::string name = "";
    std::string host = "";
    int probePort = 0;
    int secondaryPort = 0;
    bool enabled = true;
    std::string startCmd = "";
    std::string stopCmd = "";
    std::string statusCmd = "";
    std::string pidFile = "";
    std::string logFile = "";
};

struct GeminiConfig {
    bool enabled = false;
    std::string apiKey = "";
    std::string model = "gemini-2.5-flash";
    int timeoutSec = 30;
    bool autoTriageOnCrash = false;
    int maxTokens = 1024;
    double temperature = 0.2;
};

struct AimonConfig {
    PollingConfig polling;
    WebConfig web;
    MqttConfig mqtt;
    HistoryConfig history;
    AntigravityConfig antigravity;
    CursorConfig cursor;
    GatewayConfig gateway;
    SupervisorConfig supervisor;
    std::vector<SupervisedServiceConfig> services;
    GeminiConfig gemini;

    nlohmann::json toJson() const;
    void fromJson(const nlohmann::json& j);
};

class ConfigManager {
public:
    ConfigManager();
    ~ConfigManager() = default;

    bool load(const std::string& customPath = "");
    bool save(const std::string& customPath = "") const;

    const AimonConfig& getConfig() const { return _config; }
    AimonConfig& getMutableConfig() { return _config; }

    const std::string& getConfigFilePath() const { return _configFilePath; }

private:
    bool loadLibConfig(const std::string& filePath);
    bool saveLibConfig(const std::string& filePath) const;
    void applyEnvironmentOverrides();

    AimonConfig _config;
    std::string _configFilePath;
};

} // namespace aimon

#endif // AIMON_CONFIG_MANAGER_HXX
```

#### 2.4.2 `include/ProcessMonitor.hxx`
```cpp
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
    std::string actionTaken = "";
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

private:
    pid_t spawnChild(bool safeMode = false, bool usePrevBinary = false);
    MonitorAction evaluateChildExit(int status);
    bool executeDbQuarantine();
    bool executeRollback();
    void executeCloudTriage(int exitStatus, int termSignal);
    void writeIncidentLog(const CrashIncident& incident);

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
};

} // namespace aimon

#endif // AIMON_PROCESS_MONITOR_HXX
```

#### 2.4.3 `include/ServiceSupervisor.hxx`
```cpp
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
#include <shared_mutex>
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

private:
    void supervisorLoop();
    void probeService(ServiceRuntimeStatus& service);
    bool executeRemoteStart(const SupervisedServiceConfig& cfg);
    bool executeRemoteStop(const SupervisedServiceConfig& cfg);
    std::string fetchRemoteLogs(const SupervisedServiceConfig& cfg, int lines);

    SupervisorConfig _config;
    StateChangedCallback _onStateChanged;

    std::atomic<bool> _running{false};
    std::unique_ptr<std::thread> _workerThread;
    mutable std::shared_mutex _mutex;
    std::map<std::string, ServiceRuntimeStatus> _services;
};

} // namespace aimon

#endif // AIMON_SERVICE_SUPERVISOR_HXX
```

#### 2.4.4 `include/GeminiTriage.hxx`
```cpp
/*
 * GeminiTriage.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_GEMINI_TRIAGE_HXX
#define AIMON_GEMINI_TRIAGE_HXX

#include <string>
#include <cstdint>
#include <nlohmann/json.hpp>
#include "ConfigManager.hxx"

namespace aimon {

struct TriageReport {
    int64_t timestampEpoch = 0;
    std::string rootCauseCategory = "UNKNOWN";
    std::string faultingFile = "";
    int faultingLine = 0;
    std::string diagnosisText = "";
    std::string suggestedFixDiff = "";
    bool success = false;
    std::string rawApiResponse = "";

    nlohmann::json toJson() const;
};

class GeminiTriage {
public:
    GeminiTriage(const GeminiConfig& config);
    ~GeminiTriage() = default;

    bool isEnabled() const { return _config.enabled && !_config.apiKey.empty(); }

    TriageReport analyzeCrash(const std::string& binaryPath,
                              const std::string& coreDumpPath,
                              const std::string& trailingLogText,
                              int exitStatus,
                              int termSignal);

    static std::string extractGdbBacktrace(const std::string& binaryPath,
                                          const std::string& coreDumpPath);

private:
    std::string buildTriagePrompt(const std::string& backtrace,
                                 const std::string& logs,
                                 int exitStatus,
                                 int termSignal) const;

    GeminiConfig _config;
};

} // namespace aimon

#endif // AIMON_GEMINI_TRIAGE_HXX
```

### 2.5 Dual-Process Lifecycle & Socketpair IPC Protocol

1. **Anonymous IPC Stream Channel**:
   - `int fds[2]; socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds)`.
   - Parent descriptor: `_ipcSocketFds[0]`. Child descriptor: `_ipcSocketFds[1]`.
   - Token: `"SHUTDOWN\n"` (9 bytes).
2. **Signal Handling & Clean Exit Symmetry**:
   - Parent registers `SIGINT`, `SIGTERM`, `SIGHUP`. On receipt, forwards signal to child.
   - Child signal handler writes `"SHUTDOWN\n"` to IPC stream, flushes databases, and exits `0`.
   - Parent catches token, reaps child via `waitpid()`, and exits `0`.
3. **Crash Detection**:
   - If child terminates without `"SHUTDOWN\n"` and `WIFSIGNALED(status)` or non-zero exit code: parent evaluates crash policy.

### 2.6 Multi-Host Fleet Service Supervision Engine

1. **Non-Blocking TCP Socket Probing**:
   - Uses `SOCK_NONBLOCK` socket, non-blocking `connect()`, `poll(POLLOUT, probeTimeoutMs)`, and `getsockopt(SOL_SOCKET, SO_ERROR)`.
   - If primary port fails, probes `secondaryPort` before flagging failure.
2. **State Machine**:
   - `HEALTHY` (probe OK) $\rightarrow$ `DEGRADED` (1 timeout) $\rightarrow$ `RESTARTING` ($\ge 2$ timeouts, triggers async restart) $\rightarrow$ `CRASH_LOOP` ($\ge 3$ restarts in 60s).
3. **Direct Background SSH Execution**:
   - Executes remote start/stop via non-blocking `fork()` + `execlp("sh", "sh", "-c", startCmd)` redirecting output to log files. Zero `screen`.

### 2.7 Anti-Thrashing Circuit Breaker & Staged Recovery Ladder

1. **Sliding Window Crash Tracker**:
   - Prunes crash timestamps older than `crashLoopWindowSec` (60s).
   - Resets counter if continuous uptime exceeds `healthyUptimeThresholdSec` (60s).
2. **Escalation Ladder**:
   - Crash 1: Exponential backoff respawn (`backoffInitialSec` = 2s).
   - Crash 2: Exponential backoff respawn (`backoffInitialSec * 2` = 4s).
   - Crash 3: **Circuit Breaker Trips**.
     - Step 3.1: **Database Quarantine**: Atomically move `aimon.db*` to `~/.config/aimon/quarantine/<timestamp>/`.
     - Step 3.2: **Safe-Mode Boot**: Relaunch with `--safe-mode` (poller disabled, web server only).
     - Step 3.3: **Binary Rollback**: If safe-mode fails, execute `./build/aimon.prev` if executable.

### 2.8 Optional Headless Gemini Cloud Triage Architecture

1. **Strict Dormancy**:
   - Returns immediately in <5ms with 0 open network sockets if `gemini.enabled = false` or API key empty.
2. **GDB Backtrace Extraction**:
   - Captures crash backtrace via `gdb -batch -ex 'bt 50' <binary> <core>`.
3. **Direct HTTPS REST API**:
   - Connects to `generativelanguage.googleapis.com:443` via `httplib::SSLClient`.
   - Transmits structured prompt, parses JSON response, and writes incident report to disk.

### 2.9 Threat Model and Failure Invariants

- **Untrusted Satellite Nodes**: A hung satellite daemon (`fox`, `rhino`) or dropped SSH connection must never hang the central supervisor loop or block web requests. All probes and SSH executions are non-blocking with strict timeouts.
- **Corrupted Local State**: Corrupted SQLite database files or poisoned disk history are isolated into quarantine without crashing the monitor.
- **Network Partitions**: TCP probe timeouts trigger state degradation but do not cause cascading restarts if satellite host is completely unreachable.
- **Secret Isolation**: `GEMINI_API_KEY` is loaded from environment or restricted config (`0600` permissions) and never echoed to logs or MCP tool outputs.

---

## 3. Staged Implementation Plan & Acceptance Criteria

### 3.0 Mandatory Qualification Protocol & Strict Progression Gate Rule

> [!CAUTION]
> **Strict Non-Negotiable Gate Rule**:
> - **All envelopes must be qualified with proper testing.**
> - **Proper Testing Standard (All Using CppUTest)**:
>   1. **Repeatable**: Deterministic, hermetic, automated execution callable via CLI (`make test` or `./build/test/<test_binary>`) returning clean exit code 0 with zero flakiness across repeated runs.
>   2. **Coverage / Boundary / Fault**: Systematic test matrices validating nominal flows, boundary value limits (min/max extremes, buffer bounds, edge conditions), and fault injection (simulated errors, unexpected disconnections, malformed inputs, timeouts, crash/SIGSEGV recovery).
>   3. **Formalized in CppUTest Across Scopes**:
>      - **Unit Tests**: Isolated component validation (`TEST_GROUP(<Component>)`) verifying individual class contracts.
>      - **Integrated Tests**: Multi-component subsystem validation (`TEST_GROUP(Integration_<Subsystem>)`) exercising live data paths, IPC streams, and service coordination.
>      - **Regression Tests**: Continuous regression verification via `make test` executing all accumulated CppUTest test groups; any regression in previously passed tests halts progression immediately.
>   4. **Zero-Mock Discipline**: The third-party Gemini cloud REST API is the sole permitted mock in the entire codebase. All other unit, integrated, and regression tests must run against real physical OS and network primitives.
> - **If tests do not pass (100% assertions, 0 errors, 0 hangs, 0 memory leaks/zombies), implementation CANNOT advance to the next envelope under any circumstances.**
> - Bypassing tests, commenting out assertions, or progressing on partial passes is strictly prohibited.
> - **Failure Action Protocol**:
>   1. If any test assertion fails, throws an unhandled exception, segfaults, or times out: **STOP IMMEDIATELY**.
>   2. Report the raw failure facts and terminal output.
>   3. Diagnose and fix the root cause strictly within the current envelope's boundaries.
>   4. Re-execute the qualification and regression test suites until 100% pass rate is verified.
>   5. Log the verified pass output in the Appendix before touching any file belonging to the next envelope.

---

### Envelope 1: `libconfig++` Build Integration & Configuration Parser

**Goal:** Integrate `libconfig++` into CMake and implement full schema configuration parser for `aimon.cfg`.

Planned files:
- `CMakeLists.txt` — `pkg_check_modules(LIBCONFIGPP REQUIRED libconfig++)`, `pkg_check_modules(CPPUTEST REQUIRED cpputest)` (with prerequisite `libcpputest-dev`), include directories, link libraries, and test target;
- `include/ConfigManager.hxx` — `SupervisorConfig`, `SupervisedServiceConfig`, `GeminiConfig`, and `AimonConfig` data structures;
- `src/ConfigManager.cxx` — `loadLibConfig()` and `saveLibConfig()` implementation with line-numbered error handling;
- `test/TestLibconfigParser.cxx` — CppUTest qualification test suite.

- [x] Task 1.1: Add `pkg_check_modules` for `libconfig++` and `cpputest` to `CMakeLists.txt`.
  - **Target Files**: `CMakeLists.txt`
  - **Verification**: `pkg-config --modversion libconfig++` and `pkg-config --modversion cpputest` return valid versions; CMake configuration succeeds without warnings.
- [x] Task 1.2: Define typed configuration structs in `include/ConfigManager.hxx`.
  - **Target Files**: `include/ConfigManager.hxx`
  - **Verification**: Clean header compilation with zero syntax errors.
- [x] Task 1.3: Implement `ConfigManager::loadLibConfig` in `src/ConfigManager.cxx` parsing all groups.
  - **Target Files**: `src/ConfigManager.cxx`
  - **Verification**: Catches `FileIOException` and `ParseException` logging line number; loads all 11 supervisor fields and service array.
- [x] Task 1.4: Implement environment variable fallback for `GEMINI_API_KEY`.
  - **Target Files**: `src/ConfigManager.cxx`
  - **Verification**: Populates API key from `std::getenv("GEMINI_API_KEY")` when config key is empty.
- [x] Task 1.5: Implement `ConfigManager::saveLibConfig` for round-trip serialization.
  - **Target Files**: `src/ConfigManager.cxx`
  - **Verification**: Saves valid `libconfig++` syntax matching parsed values.
- [x] Task 1.6: Implement CppUTest qualification test suite `test/TestLibconfigParser.cxx`.
  - **Target Files**: `test/TestLibconfigParser.cxx`
  - **Test Matrix (CppUTest)**:
    - *Repeatable*: Hermetic setup/teardown creating temporary config files in `/tmp/aimon_test_*`.
    - *Unit Scope*: `TEST_GROUP(LibconfigParser_Unit)` tests typed struct deserialization, field validation, and default population.
    - *Integrated Scope*: `TEST_GROUP(LibconfigParser_Integrated)` tests real config file loading, round-trip serialization to disk, and environment variable overrides.
    - *Boundary Limits*: Default parameter population when optional keys omitted; empty service list; minimum/maximum port numbers (1, 65535, 0 out-of-range).
    - *Fault Injection*: Malformed syntax error catching with line numbers; non-existent file path fallback; read-only file write error handling.
  - **Verification**: `TEST_GROUP(LibconfigParser_Unit)` and `TEST_GROUP(LibconfigParser_Integrated)` pass with 100% assertions and zero memory leaks.
- [x] Task 1.7: Register and execute `./build/test_libconfig_parser`.
  - **Target Files**: `Makefile`, `CMakeLists.txt`
  - **Verification**: `./build/test_libconfig_parser` exits with code 0 and 100% assertions passing.
- [x] Task 1.8: Execute full CppUTest regression suite via `make test`.
  - **Target Files**: `Makefile`
  - **Verification**: `make test` runs all registered test targets cleanly with exit code 0.

**Hardstop:** Do not touch Envelope 2 files until `./build/test_libconfig_parser` exits 0 and `make test` passes 100% of assertions.

---

### Envelope 2: Native POSIX `aimon-monitor` Parent Process

**Goal:** Implement dedicated parent supervision process with anonymous socketpair IPC, signal forwarding, and clean exit symmetry.

Planned files:
- `include/ProcessMonitor.hxx` — `ProcessMonitor` class interface and `MonitorAction` enums;
- `src/ProcessMonitor.cxx` — parent loop, `socketpair` setup, signal handlers, child fork and waitpid;
- `src/Main.cxx` — integration of monitor fork, child worker mode, and clean shutdown IPC write;
- `test/TestProcessMonitorLifecycle.cxx` — lifecycle qualification test suite.

- [x] Task 2.1: Implement `socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC)` anonymous IPC stream in `src/ProcessMonitor.cxx`.
  - **Target Files**: `src/ProcessMonitor.cxx`
  - **Verification**: Socketpair creation succeeds; descriptors stored in `_ipcSocketFds`.
- [x] Task 2.2: Implement `ProcessMonitor::spawnChild` and child argument forwarding (`--child-worker`).
  - **Target Files**: `src/ProcessMonitor.cxx`
  - **Verification**: Child process spawns with duplicate arguments plus `--child-worker`.
- [x] Task 2.3: Implement parent supervision loop with `poll()` on IPC descriptor and `waitpid()`.
  - **Target Files**: `src/ProcessMonitor.cxx`
  - **Verification**: Parent reaps child immediately on exit; catches `"SHUTDOWN\n"` token.
- [x] Task 2.4: Implement graceful shutdown handler in `src/Main.cxx` transmitting `"SHUTDOWN\n"`.
  - **Target Files**: `src/Main.cxx`
  - **Verification**: `SIGINT` or `SIGTERM` to child triggers `"SHUTDOWN\n"` write, clean server stop, and exit 0.
- [x] Task 2.5: Implement signal forwarding (`SIGTERM`, `SIGINT`, `SIGHUP`) from parent to child.
  - **Target Files**: `src/ProcessMonitor.cxx`
  - **Verification**: Parent forwards signals to child and waits for clean exit.
- [x] Task 2.6: Implement CLI bypass flags (`--no-supervisor`, `--child-worker`).
  - **Target Files**: `src/Main.cxx`
  - **Verification**: Passing `--no-supervisor` runs standalone binary without forking parent monitor.
- [x] Task 2.7: Implement CppUTest qualification test suite `test/TestProcessMonitorLifecycle.cxx`.
  - **Target Files**: `test/TestProcessMonitorLifecycle.cxx`
  - **Test Matrix (CppUTest)**:
    - *Repeatable*: Automated parent/child fork-wait harness with clean process reaping and descriptor cleanup.
    - *Unit Scope*: `TEST_GROUP(ProcessMonitor_Unit)` validates command arguments, bypass flags, and IPC message encoders.
    - *Integrated Scope*: `TEST_GROUP(ProcessMonitor_Integrated)` tests real parent/child `socketpair` streams, clean exit symmetry (`"SHUTDOWN\n"` token write), and parent signal forwarding (`SIGTERM`, `SIGINT`).
    - *Boundary Limits*: Zero-byte IPC read, single-byte partial token read, maximum argument forwarding length.
    - *Fault Injection*: Abrupt child crash (`kill(SIGKILL)`), SIGSEGV simulation, closed socketpair descriptor handling.
  - **Verification**: `TEST_GROUP(ProcessMonitor_Unit)` and `TEST_GROUP(ProcessMonitor_Integrated)` pass with 100% assertions, 0 zombies, and code 0.
- [x] Task 2.8: Register and execute `./build/test_process_monitor_lifecycle`.
  - **Target Files**: `Makefile`, `CMakeLists.txt`
  - **Verification**: `./build/test_process_monitor_lifecycle` exits with code 0 and zero hanging/zombie processes.
- [x] Task 2.9: Execute full CppUTest regression suite via `make test`.
  - **Target Files**: `Makefile`
  - **Verification**: `make test` runs all accumulated CppUTest suites (Envelopes 1–2) with zero regressions.

**Hardstop:** Do not touch Envelope 3 files until `./build/test_process_monitor_lifecycle` exits 0 and `make test` passes 100% of assertions.

---

### Envelope 3: Anti-Thrashing Circuit Breaker, State Quarantine & Safe-Mode Flags

**Goal:** Prevent infinite respawn loops via sliding-window crash tracking, database quarantine, safe-mode boot, and binary rollback.

Planned files:
- `include/ProcessMonitor.hxx` — circuit breaker methods (`executeDbQuarantine`, `executeRollback`);
- `src/ProcessMonitor.cxx` — sliding-window timestamp tracking, quarantine execution, safe-mode spawning;
- `src/Main.cxx` — `--safe-mode` argument parsing and execution isolation;
- `test/TestCircuitBreakerQuarantine.cxx` — qualification test suite.

- [x] Task 3.1: Implement sliding-window crash timestamp tracker and uptime reset.
  - **Target Files**: `src/ProcessMonitor.cxx`
  - **Verification**: Prunes timestamps older than 60s; resets crash counter after 60s healthy runtime.
- [x] Task 3.2: Implement circuit breaker logic tripping on `crashLoopMaxRetries` within `crashLoopWindowSec`.
  - **Target Files**: `src/ProcessMonitor.cxx`
  - **Verification**: Halts respawn loop after 3 consecutive crashes within window; logs incident.
- [x] Task 3.3: Implement `executeDbQuarantine()` with atomic filesystem rename.
  - **Target Files**: `src/ProcessMonitor.cxx`
  - **Verification**: Moves `aimon.db*` to timestamped subdirectory under `~/.config/aimon/quarantine/`.
- [x] Task 3.4: Implement `--safe-mode` command-line argument and execution mode in `src/Main.cxx`.
  - **Target Files**: `src/Main.cxx`
  - **Verification**: Safe mode disables satellite polling and gateway; binds only web dashboard on port 3883.
- [x] Task 3.5: Implement binary rollback to `./build/aimon.prev` if executable exists.
  - **Target Files**: `src/ProcessMonitor.cxx`
  - **Verification**: Falls back to `./build/aimon.prev` if safe-mode fails and rollback binary exists.
- [x] Task 3.6: Implement CppUTest qualification test suite `test/TestCircuitBreakerQuarantine.cxx`.
  - **Target Files**: `test/TestCircuitBreakerQuarantine.cxx`
  - **Test Matrix (CppUTest)**:
    - *Repeatable*: Self-contained temporary directory for SQLite files and quarantine sandbox.
    - *Unit Scope*: `TEST_GROUP(CircuitBreaker_Unit)` tests sliding-window timestamp math and exponential backoff calculations.
    - *Integrated Scope*: `TEST_GROUP(CircuitBreaker_Integrated)` tests multi-crash handling, atomic DB file quarantine moves, safe-mode execution, and rollback binary execution.
    - *Boundary Limits*: Exact boundary at `crashLoopMaxRetries` (2 crashes = respawn allowed; 3rd crash = trips circuit breaker); boundary timestamp expiration at exactly 60s.
    - *Fault Injection*: Repeated child SIGSEGV crash loop; database lock during quarantine rename; missing rollback binary graceful fallback to safe-mode.
  - **Verification**: `TEST_GROUP(CircuitBreaker_Unit)` and `TEST_GROUP(CircuitBreaker_Integrated)` pass with 100% assertions and exit code 0.
- [x] Task 3.7: Register and execute `./build/test_circuit_breaker_quarantine`.
  - **Target Files**: `Makefile`, `CMakeLists.txt`
  - **Verification**: `./build/test_circuit_breaker_quarantine` exits with code 0 and 100% assertions passing.
- [x] Task 3.8: Execute full CppUTest regression suite via `make test`.
  - **Target Files**: `Makefile`
  - **Verification**: `make test` runs all accumulated CppUTest suites (Envelopes 1–3) with zero regressions.

**Hardstop:** Do not touch Envelope 4 files until `./build/test_circuit_breaker_quarantine` exits 0 and `make test` passes 100% of assertions.

---

### Envelope 4: `aimon` Fleet `ServiceSupervisor` Engine

**Goal:** Monitor satellite daemons (`meshmon`, `netmon`, `embdevenv`) via non-blocking TCP socket probes and direct SSH restart.

Planned files:
- `include/ServiceSupervisor.hxx` — `ServiceSupervisor` class, `ServiceState` enums, `ServiceRuntimeStatus` struct;
- `src/ServiceSupervisor.cxx` — probe loop, non-blocking TCP connect, state machine, background SSH dispatch;
- `src/Main.cxx` — daemon integration and lifecycle binding;
- `test/TestServiceSupervisor.cxx` — qualification test suite.

- [x] Task 4.1: Implement non-blocking TCP socket connect probe (`socket`, `connect`, `poll(POLLOUT)`, `getsockopt`).
  - **Target Files**: `src/ServiceSupervisor.cxx`
  - **Verification**: Probe measures latency < 50ms on open port; fails cleanly within `probeTimeoutMs` on closed port.
- [x] Task 4.2: Implement service state machine (`HEALTHY`, `DEGRADED`, `RESTARTING`, `CRASH_LOOP`, `DISABLED`).
  - **Target Files**: `src/ServiceSupervisor.cxx`
  - **Verification**: 1 failure transitions to `DEGRADED`; $\ge 2$ failures transition to `RESTARTING`; $\ge 3$ restarts in 60s transitions to `CRASH_LOOP`.
- [x] Task 4.3: Implement asynchronous non-blocking remote restart dispatch via background `fork()`/`execlp()`.
  - **Target Files**: `src/ServiceSupervisor.cxx`
  - **Verification**: Dispatches `startCmd` asynchronously without blocking the supervisor thread.
- [x] Task 4.4: Implement secondary port fallback probing.
  - **Target Files**: `src/ServiceSupervisor.cxx`
  - **Verification**: If primary port closed but secondary port open, marks service healthy.
- [x] Task 4.5: Implement remote log retrieval helper (`fetchRemoteLogs`).
  - **Target Files**: `src/ServiceSupervisor.cxx`
  - **Verification**: Fetches trailing 50 lines from configured log file via SSH.
- [x] Task 4.6: Integrate `ServiceSupervisor` into `aimon` daemon lifecycle in `src/Main.cxx`.
  - **Target Files**: `src/Main.cxx`
  - **Verification**: Starts supervisor thread on boot; stops cleanly on SIGINT/SIGTERM.
- [x] Task 4.7: Implement CppUTest qualification test suite `test/TestServiceSupervisor.cxx`.
  - **Target Files**: `test/TestServiceSupervisor.cxx`
  - **Test Matrix (CppUTest)**:
    - *Repeatable*: Real ephemeral loopback TCP listener (`127.0.0.1`) created and torn down in test fixtures (zero mocks).
    - *Unit Scope*: `TEST_GROUP(ServiceSupervisor_Unit)` tests service state transition matrices and latency measurement math.
    - *Integrated Scope*: `TEST_GROUP(ServiceSupervisor_Integrated)` tests live non-blocking TCP socket connect loops, secondary port fallback, and background restart process spawning.
    - *Boundary Limits*: Probe timeout exact threshold (0ms, 1000ms); maximum restart frequency throttling.
    - *Fault Injection*: Connection refused (RST), connection drop midway, unreachable host timeout, SSH command failure exit codes.
  - **Verification**: `TEST_GROUP(ServiceSupervisor_Unit)` and `TEST_GROUP(ServiceSupervisor_Integrated)` pass with 100% assertions and exit code 0.
- [x] Task 4.8: Register and execute `./build/test_service_supervisor`.
  - **Target Files**: `Makefile`, `CMakeLists.txt`
  - **Verification**: `./build/test_service_supervisor` exits with code 0 and 100% assertions passing.
- [x] Task 4.9: Execute full CppUTest regression suite via `make test`.
  - **Target Files**: `Makefile`
  - **Verification**: `make test` runs all accumulated CppUTest suites (Envelopes 1–4) with zero regressions.

**Hardstop:** Do not touch Envelope 5 files until `./build/test_service_supervisor` exits 0 and `make test` passes 100% of assertions.

---

### Envelope 5: Optional Headless Gemini Cloud Triage Client

**Goal:** Implement optional cloud AI diagnostics over direct HTTPS to Gemini REST API without IDE dependencies.

Planned files:
- `include/GeminiTriage.hxx` — `GeminiTriage` class and `TriageReport` data structures;
- `src/GeminiTriage.cxx` — GDB backtrace extraction, JSON payload builder, HTTPS client with OpenSSL;
- `test/TestGeminiTriage.cxx` — qualification test suite.

- [x] Task 5.1: Implement strict dormancy check in `src/GeminiTriage.cxx`.
  - **Target Files**: `src/GeminiTriage.cxx`
  - **Verification**: Returns immediately in <5ms with `success = false` when `gemini.enabled = false` or API key empty; 0 open sockets.
- [x] Task 5.2: Implement GDB core dump backtrace parser (`extractGdbBacktrace`).
  - **Target Files**: `src/GeminiTriage.cxx`
  - **Verification**: Parses frame index, function names, source files, and line numbers from mock GDB backtrace.
- [x] Task 5.3: Implement Gemini REST API request payload builder and response parser.
  - **Target Files**: `src/GeminiTriage.cxx`
  - **Verification**: Generates valid JSON conforming to Google Gemini REST schema (`contents.parts.text`); parses diagnosis and suggested diff.
- [x] Task 5.4: Implement HTTPS client using `httplib::SSLClient` with timeout handling.
  - **Target Files**: `src/GeminiTriage.cxx`
  - **Verification**: Handles network timeouts cleanly without hanging or throwing unhandled exceptions.
- [x] Task 5.5: Implement incident log writer saving triage report to disk.
  - **Target Files**: `src/GeminiTriage.cxx`
  - **Verification**: Writes formatted markdown incident report to `~/.config/aimon/incidents/`.
- [x] Task 5.6: Implement CppUTest qualification test suite `test/TestGeminiTriage.cxx`.
  - **Target Files**: `test/TestGeminiTriage.cxx`
  - **Test Matrix (CppUTest)**:
    - *Repeatable*: Offline mock HTTPS test server or loopback endpoint; zero external internet dependency.
    - *Unit Scope*: `TEST_GROUP(GeminiTriage_Unit)` validates GDB stack trace parsers, JSON request serializers, and model response decoders.
    - *Integrated Scope*: `TEST_GROUP(GeminiTriage_Integrated)` tests live HTTPS client timeout handling, file writing to incidents directory, and dormancy checks.
    - *Boundary Limits*: Empty backtrace string; truncated stack trace (>50 frames); zero-length API key.
    - *Fault Injection*: Strict dormancy when `enabled = false` (0 network calls); HTTP 429 rate limit response; HTTP 500 API error; network connect timeout.
  - **Verification**: `TEST_GROUP(GeminiTriage_Unit)` and `TEST_GROUP(GeminiTriage_Integrated)` pass with 100% assertions and exit code 0.
- [x] Task 5.7: Register and execute `./build/test_gemini_triage`.
  - **Target Files**: `Makefile`, `CMakeLists.txt`
  - **Verification**: `./build/test_gemini_triage` exits with code 0 and 100% assertions passing.
- [x] Task 5.8: Execute full CppUTest regression suite via `make test`.
  - **Target Files**: `Makefile`
  - **Verification**: `make test` runs all accumulated CppUTest suites (Envelopes 1–5) with zero regressions.

**Hardstop:** Do not touch Envelope 6 files until `./build/test_gemini_triage` exits 0 and `make test` passes 100% of assertions.

---

### Envelope 6: Web Dashboard, REST Endpoints & MCP Tools

**Goal:** Expose fleet supervisor management via REST API, Web UI, and Model Context Protocol.

Planned files:
- `include/WebServer.hxx`, `src/WebServer.cxx` — REST endpoints (`/api/services`, `/api/services/restart`, `/api/services/logs`, `/api/supervisor/status`);
- `include/McpServer.hxx`, `src/McpServer.cxx` — MCP tools (`service_list`, `service_status`, `service_restart`, `service_get_logs`);
- `web/app.js`, `web/index.html` — "Fleet Daemons" UI card;
- `include/WebAssets.hxx` — compiled web asset byte arrays;
- `test/TestSupervisorApiAndMcp.cxx` — qualification test suite.

- [x] Task 6.1: Add REST endpoints to `src/WebServer.cxx`.
  - **Target Files**: `src/WebServer.cxx`
  - **Verification**: HTTP GET on `/api/services` and `/api/supervisor/status` return 200 OK with valid JSON; POST `/api/services/restart` triggers restart.
- [x] Task 6.2: Register MCP tools in `src/McpServer.cxx`.
  - **Target Files**: `src/McpServer.cxx`
  - **Verification**: `service_list`, `service_status`, `service_restart`, and `service_get_logs` return valid JSON-RPC 2.0 schemas.
- [x] Task 6.3: Implement "Fleet Daemons" card in `web/app.js` and `web/index.html`.
  - **Target Files**: `web/app.js`, `web/index.html`
  - **Verification**: UI displays real-time badges (HEALTHY/DEGRADED/RESTARTING/CRASH_LOOP) and restart buttons.
- [x] Task 6.4: Rebuild embedded web assets header (`include/WebAssets.hxx`).
  - **Target Files**: `include/WebAssets.hxx`
  - **Verification**: Web assets compiled into header; embedded server serves updated JavaScript and HTML.
- [x] Task 6.5: Implement CppUTest qualification test suite `test/TestSupervisorApiAndMcp.cxx`.
  - **Target Files**: `test/TestSupervisorApiAndMcp.cxx`
  - **Test Matrix (CppUTest)**:
    - *Repeatable*: In-memory `httplib` client hitting local test server instance.
    - *Unit Scope*: `TEST_GROUP(SupervisorApi_Unit)` tests JSON-RPC schemas and route dispatchers.
    - *Integrated Scope*: `TEST_GROUP(SupervisorApi_Integrated)` tests live HTTP REST endpoints (`/api/services`, `/api/supervisor/status`) and MCP tool invocations against a running ServiceSupervisor instance.
    - *Boundary Limits*: Empty service list; maximum URI query length; unknown service name in restart request.
    - *Fault Injection*: Malformed JSON-RPC requests; POST to non-existent endpoint (404); unauthorized/invalid restart command payload.
  - **Verification**: `TEST_GROUP(SupervisorApi_Unit)` and `TEST_GROUP(SupervisorApi_Integrated)` pass with 100% assertions and exit code 0.
- [x] Task 6.6: Register and execute `./build/test_supervisor_api_and_mcp`.
  - **Target Files**: `Makefile`, `CMakeLists.txt`
  - **Verification**: `./build/test_supervisor_api_and_mcp` exits with code 0 and 100% assertions passing.
- [x] Task 6.7: Execute full CppUTest regression suite via `make test`.
  - **Target Files**: `Makefile`
  - **Verification**: `make test` runs all accumulated CppUTest suites (Envelopes 1–6) with zero regressions.

**Hardstop:** Do not touch Envelope 7 files until `./build/test_supervisor_api_and_mcp` exits 0 and `make test` passes 100% of assertions.

---

### Envelope 7: End-to-End Fleet Integration & Physical Chaos Acceptance Test

**Goal:** Verify full system fault recovery under physical conditions across `builder`, `fox`, and `rhino`.

Planned files:
- `test/TestFleetIntegration.cxx` — dedicated CppUTest integrated fleet test suite (`TEST_GROUP(FleetIntegration)`);
- `test/ChaosTestFleetRecovery.sh` — automated multi-host chaos injection runner;
- Integrated binary `./build/aimon`.

- [x] Task 7.1: Implement CppUTest integrated fleet test suite `test/TestFleetIntegration.cxx`.
  - **Target Files**: `test/TestFleetIntegration.cxx`
  - **Test Matrix (CppUTest)**:
    - *Integrated Scope*: `TEST_GROUP(FleetIntegration)` executes full multi-daemon lifecycle: simulates fleet probe loops, monitors multi-daemon heartbeats, triggers restart dispatches, and validates REST/MCP reporting under concurrent load.
    - *Boundary & Fault*: Port collision recovery, unreachable remote hosts, rapid flapping services.
  - **Verification**: `TEST_GROUP(FleetIntegration)` passes with 100% assertions and 0 memory leaks.
- [x] Task 7.2: Register and execute `./build/test_fleet_integration`.
  - **Target Files**: `Makefile`, `CMakeLists.txt`
  - **Verification**: `./build/test_fleet_integration` exits with code 0.
- [x] Task 7.3: Execute complete CppUTest regression test suite across all 7 test binaries (`make test`).
  - **Target Files**: `Makefile`
  - **Verification**: `make test` runs all CppUTest targets (`test_libconfig_parser`, `test_process_monitor_lifecycle`, `test_circuit_breaker_quarantine`, `test_service_supervisor`, `test_gemini_triage`, `test_supervisor_api_and_mcp`, `test_fleet_integration`) with 100% pass rate.
- [x] Task 7.4: Implement automated chaos test script `test/ChaosTestFleetRecovery.sh`.
  - **Target Files**: `test/ChaosTestFleetRecovery.sh`
  - **Verification**: Script exercises real process kills and validates state transitions.
- [x] Task 7.5: Verify dual-process daemonization and PID separation on `builder`.
  - **Target Files**: `./build/aimon`
  - **Verification**: Launching `./build/aimon daemon` starts both `aimon-monitor` and `aimon` with distinct PIDs.
- [x] Task 7.6: Inject SIGSEGV into child `aimon` process; verify auto-respawn within 2 seconds.
  - **Target Files**: `test/ChaosTestFleetRecovery.sh`
  - **Verification**: Parent catches SIGSEGV, increments crash counter, and auto-respawns child; port 3883 re-binds.
- [x] Task 7.7: Terminate `meshmon` on `fox` via SSH; verify auto-restart.
  - **Target Files**: `test/ChaosTestFleetRecovery.sh`
  - **Verification**: `aimon` detects closed port 16880 within 10s, transitions state to `RESTARTING`, and relaunches `meshmon` on `fox`; `meshmon` re-attaches to `aimon:3885`.
- [x] Task 7.8: Send SIGTERM to `aimon-monitor` parent; verify clean shutdown.
  - **Target Files**: `test/ChaosTestFleetRecovery.sh`
  - **Verification**: Both parent and child terminate cleanly with exit code 0; zero lingering background processes or orphaned sockets.
- [x] Task 7.9: Execute `bash test/ChaosTestFleetRecovery.sh` and verify all 4 stages pass with code 0.
  - **Target Files**: `test/ChaosTestFleetRecovery.sh`
  - **Verification**: Script logs all stages green; exits 0.

**Hardstop:** Do not declare feature complete until all CppUTest unit, integrated, and regression tests pass 100% and physical multi-host chaos tests pass 100%.

---

### 3.8 Mandatory Verification and Validation Gates Reference

#### 3.8.1 `libconfig++` Parsing & Configuration Gate (`TEST_GROUP(LibconfigParser)`)
- **Nominal Coverage**: Assert all 11 `supervisor` fields match configured values; assert array of 3 services (`meshmon`, `netmon`, `embdevenv`) deserialize with correct ports and commands.
- **Boundary Limits**: Assert default parameter population when optional keys omitted; empty service list; minimum/maximum port numbers (1, 65535).
- **Fault Injection**: Assert environment variable override `GEMINI_API_KEY` injects when file key is empty; assert malformed syntax is caught with line number and description, returning `false` without crashing.
- **Acceptance Threshold**: 100% assertions pass, 0 memory leaks, exit code 0.

#### 3.8.2 Native POSIX Monitor Lifecycle & Clean Symmetry Gate (`TEST_GROUP(ProcessMonitorLifecycle)`)
- **Nominal Coverage**: Child sending `"SHUTDOWN\n"` on `SIGINT` causes parent to reap child and exit with code 0; `--no-supervisor` and `--child-worker` cleanly bypass parent fork.
- **Boundary Limits**: Zero-byte and single-byte IPC read boundaries; maximum argument forwarding length.
- **Fault Injection**: Abrupt child crash (`SIGKILL`), simulated SIGSEGV; parent signal forwarding (`SIGTERM`) causes clean shutdown with code 0; zero zombie (`<defunct>`) or orphan processes remain.
- **Acceptance Threshold**: 100% assertions pass, 0 zombies, exit code 0.

#### 3.8.3 Anti-Thrashing Circuit Breaker & Quarantine Gate (`TEST_GROUP(CircuitBreakerQuarantine)`)
- **Nominal Coverage**: Sliding-window crash tracking; single crash auto-respawns after exponential backoff.
- **Boundary Limits**: Exactly 2 crashes permit respawn; 3rd consecutive crash within 10s trips circuit breaker and halts respawns; continuous uptime > 60s resets crash counter.
- **Fault Injection**: Repeated crash loop; database lock during `executeDbQuarantine()` atomic move into timestamped quarantine directory; missing rollback binary graceful fallback to `--safe-mode`.
- **Acceptance Threshold**: 100% assertions pass, 0 resource leaks, exit code 0.

#### 3.8.4 Fleet Service Supervision & Network Fault Gate (`TEST_GROUP(ServiceSupervisor)`)
- **Nominal Coverage**: Open loopback port transitions state to `HEALTHY` and measures latency < 50ms.
- **Boundary Limits**: Probe timeout exact threshold (0ms, 1000ms); $\ge 3$ restart triggers in 60s transitions service to `CRASH_LOOP` and throttles commands.
- **Fault Injection**: Closing port transitions state to `DEGRADED` on 1st timeout and `RESTARTING` on 2nd timeout; restart command executes asynchronously without blocking the supervisor loop; secondary port fallback succeeds when primary is closed; network RST and timeout handling.
- **Acceptance Threshold**: 100% assertions pass, 0 descriptor leaks, exit code 0.

#### 3.8.5 Headless Gemini Cloud Dormancy & Backtrace Gate (`TEST_GROUP(GeminiTriage)`)
- **Nominal Coverage**: Mock GDB backtrace accurately extracts function name, file, and line number; JSON request conforms to Google Gemini REST schema.
- **Boundary Limits**: Empty backtrace string; truncated stack trace (>50 frames); missing API key returns cleanly without network attempts.
- **Fault Injection**: `gemini.enabled = false` returns `success = false` in <5ms with 0 open network sockets; HTTP 429 rate limit and HTTP 500 API errors handled gracefully; connection timeout handling.
- **Acceptance Threshold**: 100% assertions pass, 0 open sockets, exit code 0.

#### 3.8.6 Management REST API & MCP Tools Gate (`TEST_GROUP(SupervisorApiAndMcp)`)
- **Nominal Coverage**: `GET /api/services` returns 200 OK with valid JSON array of all services; `POST /api/services/restart` returns 200 OK and dispatches supervisor restart; `GET /api/supervisor/status` returns 200 OK with uptime, crash count, and quarantine state.
- **Boundary Limits**: Empty service list serialization; maximum URI query length.
- **Fault Injection**: MCP tools `service_list`, `service_status`, `service_restart`, `service_get_logs` return valid JSON-RPC 2.0 schemas on error; malformed restart command payload rejected with 400 Bad Request.
- **Acceptance Threshold**: 100% assertions pass, 0 memory leaks, exit code 0.

#### 3.8.7 Zero IDE / Zero Screen Headless Ground Truth Gate
- Execute all test suites in headless terminal without any IDE running.
- Prove zero GNU `screen` sessions created or accessed.
- Prove zero IDE language server dependencies (`language_server_linux_x64`, `agentapi`).

#### 3.8.8 Physical Multi-Host Chaos Acceptance Gate
- Physical kill of child `aimon` process on `builder` recovered by parent within 2 seconds.
- Physical kill of `meshmon` on `fox` detected and recovered by `aimon` within 10 seconds.
- Graceful shutdown leaves zero background processes across `builder`, `fox`, and `rhino`.

---

### 3.9 Completion Definition

The architecture is freeze-ready when this plan is approved. The implementation is complete only when:
1. All envelopes execute in order without crossing a hardstop;
2. Every gate in Section 3.8 passes on physical hardware;
3. Complete CppUTest unit, integrated, and regression test suites pass cleanly with 0 failures and 0 memory leaks (`make test`);
4. Parent monitor and central daemon exhibit 100% clean exit symmetry (code 0);
5. Anti-thrashing circuit breaker halts rapid crash loops and isolates corrupted database files;
6. Satellite monitoring and auto-restart recover real failures on `fox` and `rhino` without GNU screen;
7. Gemini client remains strictly dormant when disabled;
8. All parameters configure via `libconfig++`; and
9. Implementation changes are reviewed and committed only on explicit user instruction.

---

## Appendix. Lifecycle Transition & Status Log

| Date | Previous State | New State | Lifecycle Directory | Notes / Rationale |
|---|---|---|---|---|
| Fri Sep 25 21:00:00 CST 2026 | — | `Proposed` | `plan/` | Initial service supervisor & auto-restart proposal drafted |
| Fri Sep 25 23:30:00 CST 2026 | `Proposed` | `Proposed` | `plan/` | Reconciled headless requirements: Zero IDE, Zero GNU Screen, Dedicated Forked Monitor |
| Sat Sep 26 01:15:00 CST 2026 | `Proposed` | `Proposed` | `plan/` | Integrated complete `libconfig++` schema, C++ class headers, and 7 implementation envelopes |
| Sat Sep 26 09:16:00 CST 2026 | `Proposed` | `Proposed` | `plan/` | Codified strict test qualification gate per envelope (blocking rule on failure) |
| Sat Sep 26 09:25:00 CST 2026 | `Proposed` | `Proposed` | `plan/` | Fully morphed to adhere to `plan_real_uart_passthrough.md` template (Section 0 constraints, Section 1 boundaries, Section 2 architecture, Section 3 task checklists with hardstops and gates reference, Section 4 lifecycle log) |
| Sat Sep 26 10:14:00 CST 2026 | `Proposed` | `Executing` | `plan/` | Plan approved by user; entering Agent Mode to execute Envelope 1 |
| Sat Sep 26 10:25:00 CST 2026 | `Executing` | `Executing` | `plan/` | Envelope 1 completed: libconfig++ and cpputest integrated, ConfigManager implemented, TestLibconfigParser passing 100% (7 tests, 93 checks, 0 leaks) |
| Sat Sep 26 10:37:00 CST 2026 | `Executing` | `Executing` | `plan/` | Envelope 2 completed: ProcessMonitor parent/child IPC stream, signal forwarding, and clean exit symmetry verified (10 tests, 50 checks, 0 zombies, 0 leaks) |
| Sat Sep 26 11:05:00 CST 2026 | `Executing` | `Executing` | `plan/` | Envelope 3 completed: Circuit breaker sliding-window crash tracking, SQLite quarantine, safe mode, and binary rollback verified (5 tests, 28 checks, 0 leaks) |
| Sat Sep 26 11:28:00 CST 2026 | `Executing` | `Executing` | `plan/` | Envelope 4 completed: ServiceSupervisor probe loops, state machine, secondary port fallback, and background restart verified (11 tests, 58 checks, 0 leaks) |
| Sat Sep 26 11:45:00 CST 2026 | `Executing` | `Executing` | `plan/` | Envelope 5 completed: Gemini triage client dormancy (<5ms), GDB stack trace parser, and JSON-RPC serializer verified (11 tests, 38 checks, 0 leaks) |
| Sat Sep 26 12:22:00 CST 2026 | `Executing` | `Executing` | `plan/` | Envelope 6 completed: REST API, MCP tools (service_list, service_status, service_restart, service_get_logs), Web UI card, and qualification suite verified (18 tests, 81 checks, 0 leaks) |
| Sat Sep 26 12:28:00 CST 2026 | `Executing` | `Executing` | `plan/` | Envelope 7 completed: Fleet integration suite verified (3 tests, 25 checks, 0 leaks), full regression suite passing (11 test binaries, 0 leaks), and physical chaos acceptance test passing (dual-process, SIGSEGV auto-respawn, SIGTERM clean shutdown) |
