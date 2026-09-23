# Architectural Specification & Master Implementation Plan: aimon Mobile Companion Application, Dual-Surface Action Approval Gateway & Rolling Agent Telemetry Engine

- **Author**: Charles Chiou
- **Date**: 2026-09-23
- **Status**: Proposed (Consolidated Master Plan of Record — Awaiting User Approval)
- **Scope**:
  - `aimon` C++ Host Daemon (`MobileGateway`, `AgentTelemetryDb`, `WebServer`, `TcpGateway`).
  - `third_party/antimatter` (Personal Submodule Fork: `samurai0000000/antimatter`).
  - `mobile/android` (Kotlin + Jetpack Compose Android Client with Native `NotificationCompat` & Glance AppWidget).
  - Web Dashboard (`web/index.html`, `web/app.js`, `web/style.css`, `include/WebAssets.hxx`).
  - Dual-IDE Integration: Google Antigravity (`.agents/hooks.json`) and Cursor IDE (`.cursor/hooks.json`).

---

## 1. Executive Summary & Problem Diagnosis

During autonomous AI pair programming across **Google Antigravity** and **Cursor IDE**, developers face three primary operational challenges when away from the workstation or managing long-running tasks:

1. **Unbounded Agent Gating on Tool Confirmation**: Autonomous agent workflows (e.g. multi-file refactoring, long compilation runs, embedded firmware flashing) stall indefinitely when sensitive tools (`run_command`, `write_to_file`, `multi_replace_file_content`, `embdevenv_mcu_power`) request human confirmation.
2. **Supervisory Disconnect & Remote Observability**: Developers cannot monitor streaming thought chains, verify Google AI Ultra credit burn rates, check Cursor fast request limits and billing spend, or inspect local satellite infrastructure (`meshmon`, `netmon`, `embdevenv`) from a mobile device without launching heavy, insecure desktop remote sessions.
3. **Absence of Historical Lifecycle Telemetry & Analytics**: The lack of a local rolling time-series database leaves developers blind to agent cognitive latency, P95 execution times, tool failure rates, subagent recursion depth, and long-term token velocity trends across weeks and months.

This consolidated specification details the end-to-end design and implementation for an **integrated, local-first Mobile Companion App, Action Approval Hub, and Rolling Agent Telemetry Database** built directly into **`aimon`** (modeled after the proven `SnmpDatabase` architecture in `netmon`).

---

## 2. Comprehensive ASCII Architecture & Sequence Diagrams

### 2.1 System Component & Socket Topology

```text
+--------------------------------------------------------------------------------------------------------------------+
|                                              DEVELOPER WORKSTATION (builder)                                       |
|                                                                                                                    |
|   +------------------------------------+                                   +------------------------------------+  |
|   |         Google Antigravity         |                                   |             Cursor IDE             |  |
|   |   .agents/hooks.json (Pre/PostTool)|                                   |  .cursor/hooks.json (pre/postTool) |  |
|   +-----------------+------------------+                                   +-----------------+------------------+  |
|                     |                                                                        |                     |
|                     | 1. Pipe Tool JSON on stdin                                             | 1. Pipe Tool JSON   |
|                     v                                                                        v                     |
|   +-------------------------------------------------------------------------------------------------------------+  |
|   |                       scripts/mobile_permission_relay.py (Universal Hook & Telemetry Normalizer)            |  |
|   +------------------------------------------------------+------------------------------------------------------+  |
|                                                          |                                                         |
|                                                          | 2. POST /api/approvals/request (Blocks on Promise)      |
|                                                          |    POST /api/telemetry/event   (Async Event Ingestion)  |
|                                                          v                                                         |
|   +-------------------------------------------------------------------------------------------------------------+  |
|   |                                         aimon DAEMON (Port 3883 / TLS)                                      |  |
|   |                                                                                                             |  |
|   |   +--------------------------+     +-------------------------------+     +------------------------------+   |  |
|   |   |        WebServer         |     |      MobileGateway Engine     |     |       Approval Latch         |   |  |
|   |   | - /api/mobile/pair       | <-> | - Token & QR Engine (128-bit) | <-> | - std::promise<Decision>     |   |  |
|   |   | - /api/mobile/auth       |     | - Client Session Table        |     | - 120s Timeout Reaper        |   |  |
|   |   | - /ws/mobile (WebSocket) |     | - Multicast Broadcast Engine  |     | - Immutability Table         |   |  |
|   |   | - /api/telemetry/*       |     | - Device Revocation Engine    |     | - Lock-Screen Action Tokens  |   |  |
|   |   | - /ws/telemetry          |     +---------------+---------------+     +------------------------------+   |  |
|   |   +------------+-------------+                     |                                                           |  |
|   |                |                                   |                                                           |  |
|   |                v                                   v                                                           |  |
|   |   +--------------------------+     +-------------------------------+     +------------------------------+   |  |
|   |   |     AgentTelemetryDb     |     |       StateStore Hub          |     |       TcpGateway Engine      |   |  |
|   |   | - agent_sessions         |     | - Google Ultra Credits        |     | - meshmon   (192.168.8.245)  |   |  |
|   |   | - agent_lifecycle_events |     | - Cursor Fast Reqs & $        |     | - netmon    (192.168.8.30)   |   |  |
|   |   | - telemetry_samples (14d)|     | - Realtime Quota Aggregator   |     | - embdevenv (n1-655-pro MCU) |   |  |
|   |   | - hourly_rollups (365d)  |     | - Antigravity Scraper         |     +------------------------------+   |  |
|   |   | - rollupAndPrune()       |     | - Cursor Scraper              |                                        |  |
|   |   +--------------------------+     +-------------------------------+                                        |  |
|   +----------------------------------------------------+--------------------------------------------------------+  |
|                                                        │                                                           |
|                                    Web HTTP / WS (:3883)                                                           |
|                                                        ▼                                                           |
|                     +--------------------------------------------------------------------+                         |
|                     |                Web Dashboard (:3883 / web/index.html)              |                         |
|                     |  - Token Velocity & Quota Burn SVG Graph (Prompt vs. Completion)   |                         |
|                     |  - Turn Latency & P95 Distribution Chart                           |                         |
|                     |  - Tool Invocation Matrix (Doughnut & Horizontal Bars)             |                         |
|                     |  - Live Agent Lifecycle Waterfall & Gantt Timeline                 |                         |
|                     |  - Timeframe Switcher: [1H] [24H] [7D] [30D] [1Y]                  |                         |
|                     +--------------------------------------------------------------------+                         |
+--------------------------------------------------------|-----------------------------------------------------------+
                                                         |
                                       Encrypted WireGuard / Tailscale Tunnel
                                            (or Direct Local LAN / Wi-Fi)
                                                         |
                                                         v
+--------------------------------------------------------------------------------------------------------------------+
|                                           ANDROID MOBILE COMPANION APP                                             |
|                                      (third_party/antimatter/android / Compose)                                    |
|                                                                                                                    |
|   +-------------------------------------------------------------------------------------------------------------+  |
|   | AimonWebSocketClient (OkHttp) <--- Auto-Reconnect & Ping/Pong Heartbeat (15s)                               |  |
|   +------------------------------------------------------+------------------------------------------------------+  |
|                                                          |                                                         |
|         +------------------------------------------------+------------------------------------------------+        |
|         |                                                                                                 |        |
|         v                                                                                                 v        |
|   +-------------------------------------------------------------+   +-------------------------------------------+  |
|   |           Native Android Notification Subsystem             |   |        5-Tab Jetpack Compose Interface    |  |
|   |                                                             |   |                                           |  |
|   |  +-------------------------------------------------------+  |   |  [ 🤖 Chat ]   Streaming markdown/thoughts|  |
|   |  | Lock-Screen High-Priority Heads-Up Notification       |  |   |  [ ⚡ Approvals] Rich diffs & biometric   |  |
|   |  | [!] [ANTIGRAVITY] Action Required                     |  |   |  [ 📊 Quotas ] Google credits & Cursor $  |  |
|   |  | Tool: run_command -> make -j8                         |  |   |  [ 📈 Telemetry] Token burn & turn latency|  |
|   |  |                                                       |  |   |  [ 📡 IoT ]    meshmon, netmon, embdev    |  |
|   |  |   [ APPROVE ] (Green)       [ DENY ] (Red)            |  |   +-------------------------------------------+  |
|   |  +-------------------------------------------------------+  |   | Android Home-Screen Glance AppWidget      |  |
|   |                                                             |   |  aimon | Ultra: 3,024 | Cursor: $375      |  |
|   +-------------------------------------------------------------+   +-------------------------------------------+  |
+--------------------------------------------------------------------------------------------------------------------+
```

---

### 2.2 End-to-End Action Approval Sequence Flow

```text
IDE Agent               Relay Hook              aimon (Port 3883)          Android WebSocket          User on Phone
(Antigravity/Cursor)   (mobile_relay.py)       (MobileGateway C++)        (AimonWSClient)            (Lock-screen/App)
      |                       |                       |                          |                          |
      | 1. PreToolUse Trigger |                       |                          |                          |
      |---(tool JSON on stdin)->                      |                          |                          |
      |                       | 2. Normalize & POST   |                          |                          |
      |                       |---(/api/approvals)--->|                          |                          |
      |                       |                       | 3. Create Latch & UUID   |                          |
      |                       |                       |    Store std::promise    |                          |
      |                       |                       | 4. WS Frame              |                          |
      |                       |                       |---(approval_request)---->|                          |
      |                       |                       |                          | 5. Raise Notification    |
      |                       |                       |                          |---(Vibrate & Heads-up)-->|
      |                       |                       |                          |                          |
      |                       |                       |                          |                          | 6. User Reviews &
      |                       |                       |                          |                          |    Taps [Approve]
      |                       |                       |                          | 7. PendingIntent Tap     |    (or Biometric)
      |                       |                       |                          |<--(BroadcastReceiver)----|
      |                       |                       | 8. WS Frame              |                          |
      |                       |                       |<--(approval_response)----|                          |
      |                       |                       |                          |                          |
      |                       |                       | 9. Resolve Promise       |                          |
      |                       |                       |    Record Wait Latency   |                          |
      |                       |                       |    Invalidate Token      |                          |
      |                       | 10. HTTP 200 OK       |                          |                          |
      |                       |<--({"decision":"allow"|                          |                          |
      | 11. Return JSON       |                       |                          |                          |
      |<--({"decision":"allow"|                       |                          |                          |
      |                       |                       |                          |                          |
      | 12. Execute Tool      |                       |                          |                          |
      |===(make -j8)=========>|                       |                          |                          |
      |                       |                       |                          |                          |
      | 13. PostToolUse       | 14. Async POST        |                          |                          |
      |---(execution stats)-->|---(/api/telemetry)--->|                          |                          |
      |                       |                       | 15. Store in TelemetryDb |                          |
      |                       |                       |     Broadcast /ws/telem  |                          |
```

---

### 2.3 Mobile & Lock-Screen UI Layout Mockups

```text
+----------------------------------------------------+  +----------------------------------------------------+
|                LOCK-SCREEN NOTIFICATION            |  |             IN-APP APPROVAL SCREEN TAB             |
+----------------------------------------------------+  +----------------------------------------------------+
|  [🛡️ aimon] • now                                  |  |  ⚡ Action Approvals                      (1 Active)|
|  [ANTIGRAVITY] Action Required                     |  |                                                    |
|  Tool: run_command                                 |  |  +----------------------------------------------+  |
|  Command: make -j8                                 |  |  | [ANTIGRAVITY]  make -j8           ⏱️ 114s left|  |
|  Workspace: aimon                                  |  |  | Workspace: ~/work/aimon                      |  |
|                                                    |  |  |                                              |  |
|  +-----------------------+ +--------------------+  |  |  |  $ make -j8                                  |  |
|  |   [✓] APPROVE         | |   [✕] DENY         |  |  |  |  CXX src/Main.cxx.o                          |  |
|  +-----------------------+ +--------------------+  |  |  |  CXX src/MobileGateway.cxx.o                 |  |
|                                                    |  |  |                                              |  |
+----------------------------------------------------+  |  |  +------------------+  +------------------+  |  |
                                                        |  |  | [✓] APPROVE      |  | [✕] REJECT       |  |  |
+----------------------------------------------------+  |  |  +------------------+  +------------------+  |  |
|               AI QUOTA & SPEND SCREEN              |  |  +----------------------------------------------+  |
+----------------------------------------------------+  +----------------------------------------------------+
|  Google AI Studio Ultra Credits:                   |  +----------------------------------------------------+
|  [|||||||||||||||||||||||||||         ] 3,024 / 4k |  |           AGENT TELEMETRY & METRICS TAB            |
|  Reset in: 14h 22m                                 |  +----------------------------------------------------+
|                                                    |  |  Token Velocity: 148 tok/s  |  Avg Turn: 1.42s     |
|  Cursor Fast Requests & Billing:                   |  |  P95 Latency:   2.80s      |  Error Rate: 1.8%    |
|  [||||||||||||||||||||                    ]  320   |  |                                                    |
|  On-Demand Cost: $0.00 / Spend Limit: $375.00      |  |  Active Agents: 2  |  Turns (24h): 1,420           |
+----------------------------------------------------+  +----------------------------------------------------+
```

---

### 2.4 Web Dashboard & Analytics UI Mockup

```text
 ┌────────────────────────────────────────────────────────────────────────────────────────┐
 │  AIMON — Agent Telemetry & Execution Analytics                       [DB: 4.2 MB] [1H 24H 7D 30D 1Y]│
 ├────────────────────────────────────────────────────────────────────────────────────────┤
 │ ┌──────────────────┐ ┌──────────────────┐ ┌──────────────────┐ ┌──────────────────┐    │
 │ │ Active Sessions  │ │ Token Velocity   │ │ Avg Turn Latency │ │ Tool Error Rate  │    │
 │ │       2 Active   │ │   148 Tok/s      │ │     1.42 sec     │ │     1.8 %        │    │
 │ └──────────────────┘ └──────────────────┘ └──────────────────┘ └──────────────────┘    │
 ├────────────────────────────────────────────────────────────────────────────────────────┤
 │  Token Burn Velocity & Quota Trajectory (Prompt vs. Completion)                             │
 │  ┌──────────────────────────────────────────────────────────────────────────────────┐  │
 │  │      ▲ Tokens/sec                                                                │  │
 │  │  200 ┼               ╭───╮             ╭─────────╮                               │  │
 │  │  100 ┼      ╭────────╯   ╰─────────────╯         ╰─────────                      │  │
 │  │    0 ┴──────┴────────────┴─────────────┴─────────┴─────────► Time (24h)          │  │
 │  │      ── Prompt Tokens/s (Cyan)    ── Completion Tokens/s (Magenta)               │  │
 │  └──────────────────────────────────────────────────────────────────────────────────┘  │
 ├────────────────────────────────────────────────────────────────────────────────────────┤
 │  Turn Latency & P95 Distribution         │ Tool Invocation Matrix                      │
 │  ┌────────────────────────────────────┐  │ ┌─────────────────────────────────────────┐ │
 │  │ 4.0s ┼         ▲ P95 Latency       │  │ │ run_command      ████████████ (45%)     │ │
 │  │ 2.0s ┼  ╭─╮    │                   │  │ │ view_file        ████████ (30%)         │ │
 │  │ 1.0s ┼──╯ ╰────┴── Avg Latency     │  │ │ replace_file     ████ (15%)             │ │
 │  │      00:00    08:00   16:00  24:00 │  │ │ embdevenv_mcu    ██ (6%)                │ │
 │  └────────────────────────────────────┘  │ └─────────────────────────────────────────┘ │
 ├────────────────────────────────────────────────────────────────────────────────────────┤
 │  Live Agent Lifecycle Waterfall                                                        │
 │  ┌──────────────────────────────────────────────────────────────────────────────────┐  │
 │  │ [Session #ce561] Antigravity (Gemini 2.5 Pro)                                    │  │
 │  │   ├─ Turn 1: [Prompt (0.4s)] ──► [Thinking (1.2s)] ──► [view_file: OK (0.2s)]   │  │
 │  │   ├─ Turn 2: [Thinking (2.1s)] ──► [Approval Wait: 3.4s (Phone)] ──► [run: OK]   │  │
 │  │   └─ Turn 3: [subagent_spawn: task-1] ──────────────────────────► [Running...]   │  │
 │  └──────────────────────────────────────────────────────────────────────────────────┘  │
 └────────────────────────────────────────────────────────────────────────────────────────┘
```

---

## 3. Telemetry Data Taxonomy (What Hooks & Monitors Collect)

### 3.1 Task Types & Functional Work Classification
- **Code Authoring & Refactoring**: File modification tools (`replace_file_content`, `write_to_file`, `multi_replace_file_content`), tracking affected paths, extensions (`.cxx`, `.hxx`, `.kt`, `.py`, `.md`), lines added/removed, and blast radius.
- **Codebase Exploration & Search**: Inspection tools (`view_file`, `list_dir`, `grep_search`), tracking repository reading depth and navigation patterns.
- **Build, Compilation & Testing**: Commands matching `make`, `cmake`, `gradlew`, `./gradlew`, `gcc`, `pytest`, `npm test`, tracking build targets, compiler warnings/errors, and retry cycles.
- **Embedded & Hardware Ops**: Out-of-band MCU commands (`embdevenv_mcu_power`, `embdevenv_flash_target`, `embdevenv_console_*`), Telnet serial catches, and MCU telemetry.
- **Network, Mesh & Firewall Control**: Tool calls (`firewall_block_ip`, `lan_*`, `meshmon_*`, `snmp_*`).
- **Subagent & Browser Delegation**: `browser_subagent` launches, headless browser tests, subagent recursion depth, and subagent task summaries.
- **Research & External Lookups**: `search_web`, `read_url_content`, external doc queries.
- **Planning & Architecture Artifacts**: Creation and iteration of `implementation_plan.md`, `walkthrough.md`, and `plan/` files.

### 3.2 Task Counts & Volumetrics
- **Turn Volume**: Total conversation turns per session, per hour, and per day.
- **Tool Invocations by Category**: Quantitative counts across file edits, shell commands, searches, and hardware ops.
- **Edit-to-Read Ratio**: Ratio of modifications to reads (quantifies authoring efficiency vs. exploration).
- **Subagent Spawns & Branching**: Total subagents spawned, concurrency overlap, and max call-tree depth.
- **Command Shapes & Sandbox Bypass**: Count of sandboxed executions vs. unsandboxed bypass requests requiring elevated user approval.

### 3.3 Execution Timing & Latencies (Millisecond Precision)
- **Agent Cognitive Latency (Thinking Time)**: Time from receiving user prompt / tool result to generating the subsequent tool call or textual response.
- **Tool Execution Duration (Runtime Latency)**: Exact millisecond execution time per tool (e.g. `make -j8` runtime vs. `grep_search` runtime).
- **Human Gating Latency (Approval Wait Time)**: Exact time an agent is paused waiting for human review on the Android lock-screen or CLI.
- **Turn End-to-End Latency**: Complete wall-clock turn duration (Prompt -> Think -> PreHook -> Tool Run -> PostHook -> Response).
- **Session Lifespan**: Total duration of active coding workflows from initiation to completion.
- **Latency Percentiles**: Rolling calculation of P50, P90, P95, and P99 latencies for turn times and tool execution.

### 3.4 Error, Failure & Denial Analytics
- **Tool Failure Rate**: Percentage of tool invocations returning non-zero exit codes, exceptions, or error messages.
- **Build & Compilation Retry Rate**: Number of compilation/test failures encountered before achieving a clean build.
- **Approval Rejection Rate**: Count of tool execution requests denied by the user via mobile lock-screen `[Deny]` or timeout rejections.
- **Tool Timeouts**: Number of commands exceeding execution timeouts.

### 3.5 Token Economics & Quota Velocity
- **Token Counts per Turn & Session**: Prompt tokens vs. completion tokens.
- **Token Consumption Velocity**: Real-time burn rate in `tokens/second` and `tokens/minute`.
- **Model Trajectory & Quota Exhaustion Forecasting**: Projected time remaining until Google Ultra daily caps or Cursor Fast Requests limits are exhausted based on current moving-average burn rate.

### 3.6 Context Memory & Compaction Metrics
- **Context Window Saturation**: Current token count relative to maximum model context window limit.
- **Compaction Events**: Frequency of context compaction cycles during long-running multi-agent sessions.

---

## 4. Host C++ Architecture & Rolling Database Engine

### 4.1 `include/MobileGateway.hxx`
```cpp
/*
 * MobileGateway.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_MOBILEGATEWAY_HXX
#define AIMON_MOBILEGATEWAY_HXX

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <future>
#include <chrono>
#include <nlohmann/json.hpp>

namespace aimon {

struct MobileSession {
    std::string deviceId;
    std::string deviceName;
    std::string token;
    time_t      pairedAt = 0;
    time_t      lastSeenAt = 0;
    bool        isRevoked = false;
};

enum class ApprovalVerdict {
    PENDING,
    APPROVED,
    DENIED,
    TIMED_OUT
};

struct ApprovalRequest {
    std::string approvalId;
    std::string agentType;
    std::string toolName;
    std::string workspace;
    nlohmann::json toolArgs;
    std::string reason;
    time_t requestedAt = 0;
    int timeoutSeconds = 120;
    ApprovalVerdict verdict = ApprovalVerdict::PENDING;
    std::shared_ptr<std::promise<ApprovalVerdict>> promise;
};

class MobileGateway {
public:
    static MobileGateway &getInstance();

    bool init(const std::string &dbPath = "");
    void shutdown();

    std::string createPairingQrCode();
    bool pairDevice(const std::string &pairingSecret,
                    const std::string &deviceId,
                    const std::string &deviceName,
                    std::string &outToken);
    bool authenticate(const std::string &token, std::string &outDeviceId);
    void revokeDevice(const std::string &deviceId);
    std::vector<MobileSession> listDevices();

    std::string submitApprovalRequest(const std::string &agentType,
                                      const std::string &toolName,
                                      const std::string &workspace,
                                      const nlohmann::json &toolArgs,
                                      const std::string &reason = "",
                                      int timeoutSeconds = 120);
    ApprovalVerdict waitForApproval(const std::string &approvalId, int timeoutSeconds = 120);
    bool resolveApproval(const std::string &approvalId, ApprovalVerdict verdict);

    void broadcastWsMessage(const std::string &type, const nlohmann::json &payload);

private:
    MobileGateway();
    ~MobileGateway();
    MobileGateway(const MobileGateway &) = delete;
    MobileGateway &operator=(const MobileGateway &) = delete;

    std::mutex _mutex;
    std::string _pairingSecret;
    time_t _pairingExpires = 0;
    std::map<std::string, MobileSession> _sessions;
    std::map<std::string, std::shared_ptr<ApprovalRequest>> _pendingApprovals;
};

} // namespace aimon

#endif /* AIMON_MOBILEGATEWAY_HXX */
```

### 4.2 `include/AgentTelemetryDb.hxx` (Modeled after Netmon's `SnmpDatabase`)
```cpp
/*
 * AgentTelemetryDb.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_AGENTTELEMETRYDB_HXX
#define AIMON_AGENTTELEMETRYDB_HXX

#include <string>
#include <vector>
#include <mutex>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

namespace aimon {

struct AgentLifecycleEvent {
    int64_t     timestamp = 0;
    std::string sessionId;
    std::string agentType;
    std::string eventType;
    int         stepIndex = 0;
    std::string toolName;
    double      durationMs = 0.0;
    std::string status = "OK";
    nlohmann::json detailsJson;
};

struct AgentTelemetrySample {
    int64_t timestamp = 0;
    int     activeAgents = 0;
    double  promptTokensSec = 0.0;
    double  compTokensSec = 0.0;
    double  toolCallsSec = 0.0;
    double  errorRatePct = 0.0;
    double  avgTurnLatencyMs = 0.0;
    double  p95TurnLatencyMs = 0.0;
    double  approvalWaitMs = 0.0;
};

class AgentTelemetryDb {
public:
    static AgentTelemetryDb &getInstance();

    bool open(const std::string &dbPath = "");
    void close();
    bool isOpen() const;

    bool recordSessionStart(const std::string &sessionId,
                            const std::string &convId,
                            const std::string &agentType,
                            const std::string &workspace,
                            const std::string &model);
    bool recordSessionEnd(const std::string &sessionId,
                          const std::string &status,
                          int totalTurns, int promptTokens, int compTokens,
                          int toolCalls, int errors, double avgTurnMs);

    bool insertEvent(const AgentLifecycleEvent &event);
    bool insertEventsBatch(const std::vector<AgentLifecycleEvent> &events);
    bool insertSample(const AgentTelemetrySample &sample);

    nlohmann::json queryOverview(int windowHours = 24);
    nlohmann::json queryTimeseries(const std::string &window = "24h", int maxPoints = 300);
    nlohmann::json querySessions(int limit = 50, const std::string &status = "");
    nlohmann::json querySessionEvents(const std::string &sessionId);
    nlohmann::json queryToolStats(int windowHours = 24);
    double query95thPercentileLatency(int64_t startEpoch, int64_t endEpoch);

    size_t rollupAndPrune(int rawRetentionDays = 30);
    int64_t getDatabaseSizeBytes() const;

private:
    AgentTelemetryDb();
    ~AgentTelemetryDb();
    AgentTelemetryDb(const AgentTelemetryDb &) = delete;
    AgentTelemetryDb &operator=(const AgentTelemetryDb &) = delete;

    bool initSchema();
    void prepareStatements();
    void finalizeStatements();

    std::string   _dbPath;
    sqlite3      *_db;
    mutable std::mutex _mutex;

    sqlite3_stmt *_stmtInsertEvent;
    sqlite3_stmt *_stmtInsertSample;
};

} // namespace aimon

#endif /* AIMON_AGENTTELEMETRYDB_HXX */
```

---

## 5. Dual-IDE Hook Relays & Universal Normalizer

### 5.1 `scripts/mobile_permission_relay.py`
The single universal normalizer handles both **Action Approvals** and **Telemetry Ingestion**, operating seamlessly across local Windows and remote SSH environments:

```python
#!/usr/bin/env python3
"""
mobile_permission_relay.py
Universal Permission & Telemetry Interceptor for Antigravity & Cursor IDE.

Copyright (C) 2026, Charles Chiou
"""

import sys
import json
import os
import urllib.request
import urllib.error
import time

SENSITIVE_TOOLS = {
    "run_command", "replace_file_content", "write_to_file",
    "multi_replace_file_content", "embdevenv_mcu_power",
    "embdevenv_flash_target", "firewall_block_ip", "firewall_unblock_ip"
}

def resolve_aimon_url():
    if "AIMON_ENDPOINT" in os.environ:
        return os.environ["AIMON_ENDPOINT"].rstrip("/")
    for url in ["http://127.0.0.1:3883", "http://builder:3883", "http://192.168.8.39:3883"]:
        try:
            req = urllib.request.Request(f"{url}/api/status", headers={"User-Agent": "aimon-hook/1.0"})
            with urllib.request.urlopen(req, timeout=0.15) as res:
                if res.status == 200:
                    return url
        except Exception:
            continue
    return "http://127.0.0.1:3883"

def main():
    raw_input = sys.stdin.read()
    if not raw_input.strip():
        sys.exit(0)

    try:
        payload = json.loads(raw_input)
    except Exception:
        sys.exit(0)

    tool_name = payload.get("tool_name") or payload.get("tool") or ""
    tool_args = payload.get("tool_args") or payload.get("args") or {}
    agent_type = "antigravity" if "tool_name" in payload else "cursor"
    session_id = payload.get("session_id", "default")
    workspace = payload.get("cwd") or payload.get("workspace_path") or os.getcwd()

    aimon_url = resolve_aimon_url()

    # 1. Asynchronously emit PreToolUse telemetry event
    try:
        telem_req = urllib.request.Request(
            f"{aimon_url}/api/telemetry/event",
            data=json.dumps({
                "session_id": session_id,
                "agent_type": agent_type,
                "event_type": "TOOL_PRE_USE",
                "tool_name": tool_name,
                "status": "OK",
                "timestamp": int(time.time()),
                "details_json": json.dumps({"workspace": workspace})
            }).encode("utf-8"),
            headers={"Content-Type": "application/json", "User-Agent": "aimon-hook/1.0"}
        )
        urllib.request.urlopen(telem_req, timeout=0.2)
    except Exception:
        pass

    # 2. Check if tool requires human approval
    if tool_name not in SENSITIVE_TOOLS:
        print(json.dumps({"decision": "allow"}))
        sys.exit(0)

    # 3. Request approval from MobileGateway
    req_body = {
        "agent_type": agent_type,
        "tool_name": tool_name,
        "workspace": workspace,
        "tool_args": tool_args,
        "reason": f"Execution of {tool_name} requires developer approval"
    }

    try:
        req = urllib.request.Request(
            f"{aimon_url}/api/approvals/request",
            data=json.dumps(req_body).encode("utf-8"),
            headers={"Content-Type": "application/json", "User-Agent": "aimon-hook/1.0"}
        )
        with urllib.request.urlopen(req, timeout=125.0) as res:
            resp_data = json.loads(res.read().decode("utf-8"))
            if resp_data.get("verdict") == "APPROVED":
                print(json.dumps({"decision": "allow"}))
                sys.exit(0)
            else:
                print(json.dumps({"decision": "deny", "reason": "Denied by user on mobile device"}))
                sys.exit(1)
    except Exception:
        # Fallback gracefully so agent is never stuck
        print(json.dumps({"decision": "allow"}))
        sys.exit(0)

if __name__ == "__main__":
    main()
```

---

## 6. Android Companion App Customization (`third_party/antimatter`)

### 6.1 Submodule Fork Strategy
Fork `saifmukhtar/antimatter` to `samurai0000000/antimatter` and bind as official git submodule:
```bash
git submodule add https://github.com/samurai0000000/antimatter third_party/antimatter
```

### 6.2 Native Android `NotificationCompat` with Action Buttons
`AimonNotificationManager.kt` raises high-priority heads-up notifications directly with system `PendingIntent`s:
```kotlin
package com.selfso.aimon.notification

import android.app.*
import android.content.Context
import android.content.Intent
import androidx.core.app.NotificationCompat

class AimonNotificationManager(private val context: Context) {
    fun raiseApprovalNotification(approvalId: String, agent: String, tool: String, summary: String) {
        val approveIntent = Intent(context, ApprovalReceiver::class.java).apply {
            action = "ACTION_APPROVE"
            putExtra("APPROVAL_ID", approvalId)
        }
        val denyIntent = Intent(context, ApprovalReceiver::class.java).apply {
            action = "ACTION_DENY"
            putExtra("APPROVAL_ID", approvalId)
        }

        val approvePending = PendingIntent.getBroadcast(
            context, approvalId.hashCode(), approveIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )
        val denyPending = PendingIntent.getBroadcast(
            context, approvalId.hashCode() + 1, denyIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val builder = NotificationCompat.Builder(context, "AIMON_APPROVALS")
            .setSmallIcon(android.R.drawable.ic_lock_lock)
            .setContentTitle("[$agent] Action Approval Required")
            .setContentText("$tool: $summary")
            .setStyle(NotificationCompat.BigTextStyle().bigText("Tool: $tool\nCommand/Args: $summary"))
            .setPriority(NotificationCompat.PRIORITY_MAX)
            .setCategory(NotificationCompat.CATEGORY_ALARM)
            .addAction(android.R.drawable.checkbox_on_background, "Approve", approvePending)
            .addAction(android.R.drawable.ic_delete, "Deny", denyPending)
            .setAutoCancel(true)

        val nm = context.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        nm.notify(approvalId.hashCode(), builder.build())
    }
}
```

---

## 7. Web Dashboard UI Analytics & Visualizations

`web/index.html`, `web/app.js`, and `web/style.css` are updated to render responsive glassmorphic charts:
- **Token Velocity Chart**: Inbound Prompt vs. Outbound Completion throughput (Cyan vs. Magenta).
- **Latency & P95 Distribution**: Turn latency area chart with shaded 95th percentile envelope.
- **Tool Invocation Doughnut & Bars**: Visual distribution of tools by category and error rate.
- **Live Agent Lifecycle Waterfall**: Interactive timeline showing turns, thinking time, tool execution, and mobile approval pauses.
- **Timeframe Selector**: Instant switching between `1h`, `24h`, `7d`, `30d`, and `1y`.

---

## 8. Staged Implementation Roadmap & Verification Gates

The implementation is structured into **6 sequential logical phases**. Each phase concludes with mandatory unit, regression, and integration test validation before progressing to the next. The mobile application and live human verification is situated in the final phase.

```
 ┌────────────────────────────────────────────────────────────────────────────┐
 │ PHASE 1: Rolling Database & Agent Telemetry Subsystem (AgentTelemetryDb)   │
 │   - SQLite WAL schemas, downsampled timeseries engine, rollupAndPrune()    │
 │   ► Gate 1: TestAgentTelemetryDb unit test + full regression tests green   │
 └─────────────────────────────────────┬──────────────────────────────────────┘
                                       │
                                       ▼
 ┌────────────────────────────────────────────────────────────────────────────┐
 │ PHASE 2: Mobile Gateway Engine & Approval Latching Subsystem               │
 │   - MobileGateway, pairing tokens, std::promise latches, 120s timeout      │
 │   ► Gate 2: TestMobileGateway unit test (threading, timeouts, replay)      │
 └─────────────────────────────────────┬──────────────────────────────────────┘
                                       │
                                       ▼
 ┌────────────────────────────────────────────────────────────────────────────┐
 │ PHASE 3: WebServer Endpoints, WebSocket Streams & Dual-IDE Hook Relays     │
 │   - /api/telemetry/*, /api/approvals/*, /ws/*, mobile_permission_relay.py  │
 │   - Passive transcript tailing in AntigravityCollector & CursorCollector   │
 │   ► Gate 3: Integration test harness (synthetic hooks, WS streaming)       │
 └─────────────────────────────────────┬──────────────────────────────────────┘
                                       │
                                       ▼
 ┌────────────────────────────────────────────────────────────────────────────┐
 │ PHASE 4: Web Dashboard Analytics & Visualizations                          │
 │   - Interactive SVG/Canvas charts (Token Velocity, P95, Tool Matrix, Gantt)│
 │   - Timeframe toggles (1H, 24H, 7D, 30D, 1Y) & WebAssets.hxx update        │
 │   ► Gate 4: Headless Chromium rendering & visual regression test           │
 └─────────────────────────────────────┬──────────────────────────────────────┘
                                       │
                                       ▼
 ┌────────────────────────────────────────────────────────────────────────────┐
 │ PHASE 5: Documentation & Architecture Synchronization                      │
 │   - Update doc/Design.md (ASCII diagrams & schemas) and README.md          │
 │   ► Gate 5: Style, BSD modeline, copyright headers & build sanity check    │
 └─────────────────────────────────────┬──────────────────────────────────────┘
                                       │
                                       ▼
 ┌────────────────────────────────────────────────────────────────────────────┐
 │ PHASE 6: Android Companion App Customization & APK Delivery (Human Gate)   │
 │   - Submodule fork (third_party/antimatter), NotificationCompat buttons    │
 │   - 5-tab Compose UI, Glance AppWidget, APK assembly (./gradlew)           │
 │   ► Gate 6: End-to-end QR pairing, lock-screen approval, live telemetry   │
 └────────────────────────────────────────────────────────────────────────────┘
```

### Phase 1: Rolling Database & Agent Telemetry Subsystem (`AgentTelemetryDb`)
- **Deliverables**:
  - `include/AgentTelemetryDb.hxx` and `src/AgentTelemetryDb.cxx`.
  - Schema initialization (`agent_sessions`, `agent_lifecycle_events`, `agent_telemetry_samples`, `agent_hourly_rollups`).
  - Downsampling time-series query generator (`queryTimeseries(window, maxPoints)`).
  - Background rollup and pruning engine (`rollupAndPrune()`).
  - Update `CMakeLists.txt`.
- **Test Gate 1**:
  - Create and compile `test/TestAgentTelemetryDb.cxx`.
  - Execute `make clean && make -j$(nproc) && ./build/test_agent_telemetry_db`.
  - Validate 100% pass on session tracking, transaction batching, downsampling, and rollups.

### Phase 2: Mobile Gateway Engine & Approval Latching Subsystem (`MobileGateway`)
- **Deliverables**:
  - `include/MobileGateway.hxx` and `src/MobileGateway.cxx`.
  - Device session table, QR secret generation, single-use 128-bit authentication tokens, and revocation.
  - Asynchronous approval latching with `std::promise<ApprovalVerdict>`, 120s timeout reaper, and token invalidation.
  - Ingestion of approval response latencies into `AgentTelemetryDb`.
  - Update `CMakeLists.txt`.
- **Test Gate 2**:
  - Create and compile `test/TestMobileGateway.cxx`.
  - Execute `make test` / `./build/test_mobile_gateway`.
  - Validate multi-threaded approval latch resolution, 120s timeout reaper fallback, and replay attack defense.

### Phase 3: WebServer Endpoints, WebSocket Streams & Dual-IDE Hook Relays
- **Deliverables**:
  - Implement `/api/telemetry/*`, `/api/approvals/*`, `/api/mobile/*`, `/ws/telemetry`, and `/ws/mobile` in `src/WebServer.cxx`.
  - Implement `scripts/mobile_permission_relay.py` with multi-tier endpoint auto-discovery and non-blocking timeout fallback.
  - Update `src/AntigravityCollector.cxx` and `src/CursorCollector.cxx` to passively ingest `transcript.jsonl` lifecycle events.
  - Configure `.agents/hooks.json` and `.cursor/hooks.json`.
- **Test Gate 3**:
  - Execute integration test harness: simulate IDE hook `PreToolUse` -> `POST /api/approvals/request` -> `POST /api/approvals/decision` -> verify HTTP response and WS broadcast.
  - Run full regression test suite (`make test`).

### Phase 4: Web Dashboard Analytics & Visualizations
- **Deliverables**:
  - Update `web/index.html`, `web/style.css`, and `web/app.js` with SVG/Canvas charts for Token Velocity, Turn Latency & P95, Tool Invocation Matrix, and Live Lifecycle Waterfall.
  - Update `include/WebAssets.hxx` compiled fallback assets.
- **Test Gate 4 (Headless Chrome Visual & Functional Validation)**:
  - Start local test daemon on port 3883 with synthetic multi-turn session data.
  - Launch native Linux headless Chromium directly on `builder` to capture full-viewport (1920x1080) screenshots across all timeframe views (`1h`, `24h`, `7d`, `30d`):
    ```bash
    /home/samurai/.cache/ms-playwright/chromium-1200/chrome-linux64/chrome \
      --headless=new --no-sandbox --disable-gpu --window-size=1920,1080 \
      --screenshot=/tmp/aimon_web_dashboard_1080p.png http://127.0.0.1:3883
    ```
  - Inspect generated screenshots to validate:
    1. Zero JavaScript/CSS console errors.
    2. Correct rendering of SVG token burn and latency curves.
    3. Proper alignment of the Gantt waterfall lifecycle timeline blocks.
    4. Glassmorphism dark-mode contrast and responsiveness.

### Phase 5: Documentation & Architecture Synchronization
- **Deliverables**:
  - Update `doc/Design.md` with complete subsystem architecture, ASCII diagrams, schemas, and API tables.
  - Update `README.md` with developer guide and hook setup instructions.
  - Verify BSD modeline headers and copyright `Copyright (C) 2026, Charles Chiou` on all new files.
- **Test Gate 5**:
  - Code inspection & build sanity check (`make clean && make -j$(nproc)`).

### Phase 6: Android Companion App Customization & APK Delivery (Human Gate)
- **Deliverables**:
  - Fork `saifmukhtar/antimatter` to `samurai0000000/antimatter` and bind as submodule `third_party/antimatter`.
  - Customize Android package `com.selfso.aimon`.
  - Implement `AimonNotificationManager.kt` (`[Approve]` / `[Deny]` action buttons), Compose tabs (`ChatScreen`, `ApprovalsScreen`, `QuotasScreen`, `TelemetryScreen`), and `AimonGlanceWidget`.
  - Assemble APK via `./gradlew assembleDebug` and host at `/download/aimon-companion.apk`.
- **Test Gate 6 (Live Human Verification)**:
  - Download and install APK on physical Android device / emulator.
  - Scan QR code from `http://builder:3883` to pair.
  - Trigger sensitive tool in Antigravity (`make -j8`); verify phone notification vibrates, tap `[Approve]`, and confirm agent unblocks immediately.

---

## 9. Deployment & Operator Workflow

### 9.1 Build & Redeployment in Screen Session `aimon`
```bash
# 1. Clean build on builder
make clean && make -j$(nproc)

# 2. Restart inside persistent screen session 'aimon'
screen -S aimon -X stuff $'\003'
screen -S aimon -X stuff "./build/aimon daemon --port 3883\n"

# 3. Verify health
curl -s http://127.0.0.1:3883/api/status | jq .
```

### 9.2 Android Companion Pairing Workflow
1. Operator launches `aimon` mobile app and scans the QR code generated at `http://builder:3883/settings` or CLI `aimon pair`.
2. App stores the 128-bit authentication token in Android KeyStore.
3. App connects to `wss://builder:3883/ws/mobile` (or over WireGuard/Tailscale VPN).
4. When Antigravity or Cursor runs a gated tool, a high-priority notification vibrates on the operator's phone lock screen.
5. Operator taps `[Approve]`, the promise resolves instantly in C++, and the tool proceeds.

---

## 10. Verification Plan

### 10.1 Automated Unit Tests
```bash
make test
./build/test_mobile_gateway
./build/test_agent_telemetry_db
```

### 10.2 Live End-to-End Tests
1. **Synthetic Telemetry Ingestion**: Ingest 1,000 multi-turn synthetic sessions and verify downsampled 300-point time-series queries.
2. **Lock-Screen Approval Flow**: Trigger `run_command: make -j8` in Antigravity; confirm heads-up notification appears on phone, tap `[Approve]`, and verify tool execution.
3. **Headless Browser Validation**: Render web dashboard with headless Chromium and verify 0 console errors.

---

## Appendix A. Phase Status & Test Execution Ledger

*Append-only record of phase execution results, compiler outputs, and automated test suite logs.*

### Phase 1: Rolling Database & Agent Telemetry Subsystem (`AgentTelemetryDb`)
- **Status**: COMPLETED & PASSED (Gate 1)
- **Execution Date**: 2026-09-23
- **Deliverables**:
  - `include/AgentTelemetryDb.hxx` and `src/AgentTelemetryDb.cxx` (SQLite WAL schemas, dynamic downsampling engine, `rollupAndPrune()`).
  - Unit test suite `test/TestAgentTelemetryDb.cxx`.
- **Test Results**:
  - `test_agent_telemetry_db`: `testBasicOperations` [PASS], `testPercentileAndRollup` [PASS].
  - 100% pass on session tracking, transaction batching, downsampling, and rollups.

### Phase 2: Mobile Gateway Engine & Approval Latching Subsystem (`MobileGateway`)
- **Status**: COMPLETED & PASSED (Gate 2)
- **Execution Date**: 2026-09-23
- **Deliverables**:
  - `include/MobileGateway.hxx` and `src/MobileGateway.cxx` (128-bit pairing secrets, token authentication, `std::promise<ApprovalVerdict>` latches, 120s timeout reaper, single-use replay defense).
  - Unit test suite `test/TestMobileGateway.cxx`.
- **Test Results**:
  - `test_mobile_gateway`: `testPairingAndAuth` [PASS], `testApprovalLatching` [PASS], `testApprovalTimeout` [PASS].
  - 100% pass on multi-threaded approval latch resolution, 120s timeout reaper fallback, and replay attack defense.

### Phase 3: WebServer Endpoints, WebSocket Streams & Dual-IDE Hook Relays
- **Status**: COMPLETED & PASSED (Gate 3)
- **Execution Date**: 2026-09-23
- **Deliverables**:
  - Implemented `/api/telemetry/*`, `/api/approvals/*`, `/api/mobile/*`, `/ws/telemetry`, and pre-routing hook bypass in `src/WebServer.cxx`.
  - Implemented universal hook relay `scripts/mobile_permission_relay.py` for Antigravity and Cursor.
  - Integration test suite `test/TestWebServerApi.cxx`.
- **Test Results**:
  - Full regression test suite (`make test`): All 4 test executables passed (`test_gateway_timeout`, `test_agent_telemetry_db`, `test_mobile_gateway`, `test_web_server_api`).

### Phase 4: Web Dashboard Analytics, Action Approvals & E2E Verification
- **Status**: COMPLETED & FULLY VERIFIED (Gate 4)
- **Execution Date**: 2026-09-23
- **Deliverables**:
  - Preserved existing `aimon` front page (`#view-aimon` / AI Quotas Self) 100% untouched as the default tab.
  - Implemented extra tabs: `Agent Telemetry` (`#view-telemetry`) and `Action Approvals` (`#view-approvals`).
  - Added pure vanilla JavaScript QR Code SVG Generator (`web/qrcode.js`) with Byte Mode and Reed-Solomon GF(256) error correction for visual glowing cyan QR matrices.
  - Interactive Action Approval Workflow: Live pending approval cards with countdown timers, `[Approve Action]` and `[Deny]` action buttons, resolving C++ `std::promise` in real time.
  - Live Agent Lifecycle Waterfall: Session dropdown selection, chronological execution flow with colored tags (`USER_TURN` in purple, `THINKING` in cyan, `TOOL_CALL` in green, `APPROVAL_WAIT` in amber pulse, `ERROR` in red), duration metrics, and scrollable container.
  - Interactive Spend Chart & Model Breakdown on Cursor card: SVG trend line, gradient fill area, and hover tooltips on data dots.
  - Timeframe Selector: Dynamic switching across `1H`, `24H`, `7D`, `30D`, `1Y`.
  - Satellite Monitors (`netmon`, `meshmon`, `embdevenv`): Toolbar with online/offline dots, name, badge, resolved URL, `[Reload]` button, and offline overlay fallback.
- **E2E Automated Verification Test Results**:
  - Executed automated browser test suite (`scripts/verify_e2e_ui.mjs`) on headless Chromium directly on builder (`/home/samurai/.cache/ms-playwright/chromium-1200/chrome-linux64/chrome` via WebSocket CDP port 9335):
    1. **AI Quotas Tab**: Verified quota groups, individual models toggle (`#ag-models-toggle`), spend chart dots, MCP clients list, and force refresh button (`#refresh-btn`).
    2. **Agent Telemetry Tab**: Verified 4 KPI cards, tool category bars, timeframe buttons (`1h`, `7d`, `30d`, `1y`, `24h`), session dropdown switching (`#waterfall-session-select`), and waterfall execution track.
    3. **Action Approvals Tab**: Verified visual QR code SVG generation (`viewBox="0 0 41 41"`), alphanumeric secret code display, countdown, and secret regeneration button (`#btn-new-qr`).
    4. **Approval Action Execution**: Created live test approval (`run_command`), confirmed pending card appeared on UI, clicked `[Approve Action]`, verified approval resolved with verdict `APPROVED`, and card cleared.
    5. **Satellite Monitor Tabs**: Verified tab navigation to `netmon`, `meshmon`, `embdevenv`, verified iframe toolbar state, and clicked `[Reload]` buttons.
  - Zero JavaScript / CSS console errors across all interactions.

### Phase 4.1: Web Dashboard & Telemetry Usability Remediation (QA Hardening)
- **Status**: IN PROGRESS / PLANNED
- **Deliverables**:
  - **Latency Chart Overhaul (`web/app.js`, `web/index.html`)**:
    - Add color swatch legend indicators (`.dot-blue` Avg Duration, `.dot-amber` P95 Envelope).
    - Relabel "Max Duration (ms)" to "P95 Envelope".
    - Add explanatory subtitle for latency health thresholds (< 3s Normal, 3–8s Moderate, > 8s Degraded).
    - Increase Y-axis left padding (`padL = 65`) and format numbers dynamically (`ms`, `s`, `m`).
    - Generate X-axis temporal ticks and timestamps.
    - Implement interactive hover crosshairs and floating tooltip overlay.
  - **Workload Velocity Dual Y-Axis (`web/app.js`)**:
    - Left axis: Tool Invocations (`calls/s`) in Cyan.
    - Right axis: Active Sessions count in Magenta (clean integer scale 0, 1, 2, 3+).
    - X-axis temporal ticks and hover tooltips.
  - **Responsive Viewport & Mobile Breakpoints (`web/style.css`, `web/app.js`)**:
    - Mobile horizontal touch scrolling on `.monitor-nav` with scroll hints.
    - Navbar layout stacking fixes on narrow viewports (< 600px).
    - Prevent date label collisions on Cursor spend chart on small screens.
    - Prevent collapse of `.tool-bar-track` on mobile screens.
    - Eliminate dual scrollbars on laptop viewports by calculating `height: calc(100vh - 150px)` for iframes.
  - **Dead DOM Cleanup & Polish (`web/app.js`)**:
    - Remove dead `#kpi-token-velocity` lookup.
    - Refine waterfall empty state when selected session has 0 events.
    - Add full-name hover tooltips on truncated tool matrix rows.
  - **Favicon & Network Efficiency (`src/WebServer.cxx`, `web/app.js`)**:
    - Serve inline SVG favicon on `GET /favicon.ico`.
    - Back off background polling intervals.

### Phase 5: Documentation & Architecture Synchronization
- **Status**: COMPLETED & PASSED (Gate 5)
- **Execution Date**: 2026-09-23
- **Deliverables**:
  - Updated `doc/Design.md` with complete architectural sections for `AgentTelemetryDb`, `MobileGateway`, Web Dashboard, Hook Relays, and Android Companion.
  - Updated `README.md` with new project structure, modules, scripts, and test suites.
  - Verified BSD modeline headers and `Copyright (C) 2026, Charles Chiou` on all new files.
  - Verified clean compile (`make clean && make -j$(nproc) && make test` - 100% green across all 4 test suites).

### Phase 6: Android Companion App Customization & APK Delivery (Human Gate)
- **Status**: Pending Human Interaction
- **Target Deliverables**:
  - Fork `saifmukhtar/antimatter` to `samurai0000000/antimatter` as submodule `third_party/antimatter`.
  - Android package `com.selfso.aimon` with `AimonNotificationManager.kt` (`[Approve]`/`[Deny]` action buttons), Compose tabs, and Glance AppWidget.
  - Assemble APK (`./gradlew assembleDebug`) and end-to-end QR pairing test.


