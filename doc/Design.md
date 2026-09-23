# aimon: Architectural Design & Technical Specification

## 1. Executive Summary & Problem Statement

### 1.1 Context
Modern software engineering workflows increasingly rely on AI pair programming assistants such as **Google Antigravity** and **Cursor**. Both platforms enforce subscription tiers and rate limits:
* **Antigravity** employs a dual-limit rolling quota architecture: a short-term rolling window (5-hour refresh) alongside weekly model group caps.
* **Cursor** enforces monthly fast-request pools, usage-based caps, and billing period cycles.

### 1.2 The Problem
1. **Network-Restricted Web Access**: In enterprise configurations, Cursor's web dashboard (`cursor.com/settings`) is frequently protected by corporate Single Sign-On (SSO) and strict IP-allowlisting policies. Developers working remotely or off-network cannot access the web dashboard to inspect remaining requests or billing status.
2. **Quota Invisibility in Editors**: Antigravity's weekly limits and specific model quota states are not always prominent in the main IDE editing canvas, risking abrupt mid-sprint lockouts.
3. **Fragmented Subsystem Interfaces**: Engineering environments feature specialized background daemons like `meshmon` (LoRa mesh RF monitor) and `netmon` (firewall, packet trace, SNMP, and LAN sniffer). Having each daemon attempt to run standalone MCP servers forces AI agents to manage multiple endpoints and stdio child processes, complicating deployment across heterogeneous hardware.

### 1.3 The Solution
`aimon` operates as a unified, privacy-first local monitoring engine and **Central MCP Tool Gateway Hub**:
1. **AI Quota & Subscription Monitoring**: Directly gathers metrics from the Cursor Desktop API (`api2.cursor.sh`) and Antigravity Language Server loopback RPC (`127.0.0.1`).
2. **Central MCP Tool Gateway**: Acts as the sole MCP integration point for AI agents (Antigravity and Cursor) over HTTP Server-Sent Events (SSE on port `3883`).
3. **Multiplexed TCP Gateway for Subsystems**: Runs a TCP gateway server on port `3885` accepting connections from specialized network daemons (e.g. `meshmon` LoRa radio gateway, `netmon` LAN monitor). When daemons connect, they dynamically register their toolsets; `aimon` merges these tools into its global MCP registry and proxies RPC invocations transparently.
4. **Embedded Web & Home Assistant Interfaces**: Real-time browser dashboard (`http://localhost:3883`) and native Home Assistant MQTT Auto-Discovery.

---

## 2. Distributed Hub-and-Spoke System Architecture

```text
┌─────────────────────────────────────────────────────────────────────────────┐
│                       AI Assistant / Agent Client                           │
│                    (IDE Agents, Autonomous CLI, LLMs)                       │
└──────────────────────────────────────┬──────────────────────────────────────┘
                                       │ MCP over SSE (http://<host>:3883/sse)
                                       │ (JSON-RPC 2.0)
                                       ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                         aimon  (Central MCP Gateway)                        │
│                                                                             │
│   ┌───────────────────────────────────┐  ┌──────────────────────────────┐   │
│   │ Native AI Quota Toolset           │  │ Dynamic Toolset Registry     │   │
│   │  - check_antigravity_quota        │  │  - Subsystem tool schemas    │   │
│   │  - check_cursor_usage             │  │  - Scoped MCP Profiling      │   │
│   │  - get_combined_ai_status         │  │  - Dynamic RPC routing table │   │
│   │                                   │  │  - Tool state notifications  │   │
│   │                                   │  │  - Lifecycle / heartbeats    │   │
│   └───────────────────────────────────┘  └──────────────┬───────────────┘   │
│                                                         │                   │
│   ┌─────────────────────────────────────────────────────┴───────────────┐   │
│   │                    TCP Gateway Server (:3885)                       │   │
│   │               Line-delimited JSON-RPC 2.0 Multiplexer               │   │
│   └──────────────────────────┬──────────────────────────┬───────────────┘   │
└──────────────────────────────┼──────────────────────────┼───────────────────┘
                               │                          │
          Line-delimited       │                          │ Line-delimited
          JSON-RPC 2.0 / TCP   │                          │ JSON-RPC 2.0 / TCP
                               ▼                          ▼
┌───────────────────────────────────────────────┐ ┌───────────────────────────┐
│              meshmon Daemon                   │ │       netmon Daemon       │
│      (Radio Gateway & Mesh Telemetry)         │ │(Network Observation & Sec)│
│                                               │ │                           │
│ - Hardware: Meshtastic LoRa Transceivers      │ │ - Core LAN Sniffer NIC    │
│ - Asynchronous SQLite Packet Logging (DB)     │ │ - Pluggable RouterDriver  │
│ - Deep RF & Mesh Analytics (SPOF, Echo, SNR)  │ │ - AI Security Gatekeeper  │
│ - Home Assistant MQTT & Device Automation     │ │ - Packet Trace DB & SNMP  │
│ - Gateway TCP Client (Exports meshmon_* tools)│ │ - Gateway TCP Client      │
└───────────────────────────────────────────────┘ └───────────────────────────┘
```

---

## 3. TCP Tool Gateway Protocol Specification

Communication between `aimon` and connected satellite daemons (`meshmon`, `netmon`) occurs over a dedicated TCP port (`3885` by default) using newline-delimited JSON-RPC 2.0 messages.

### 3.1 Handshake & Tool Registration (`gateway/register`)
Upon establishing a TCP connection, the satellite daemon sends a `gateway/register` request containing its identity and array of supported MCP tools:

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "gateway/register",
  "params": {
    "subsystem": "meshmon",
    "version": "1.0.0",
    "hostname": "lora-gateway",
    "tools": [
      {
        "name": "meshmon_get_node_status",
        "description": "Returns status, battery, SNR, and telemetry for Meshtastic nodes in the mesh.",
        "inputSchema": {
          "type": "object",
          "properties": {
            "node_id": {"type": "string", "description": "Optional node hex ID (e.g. !1234abcd) or node name. Returns all if omitted."}
          }
        }
      },
      {
        "name": "meshmon_send_message",
        "description": "Transmits a text message to a specific node or broadcasts to the entire mesh.",
        "inputSchema": {
          "type": "object",
          "properties": {
            "destination": {"type": "string", "description": "Target hex ID or ^all for broadcast"},
            "message": {"type": "string", "description": "Text message payload"}
          },
          "required": ["message"]
        }
      }
    ]
  }
}
```

`aimon` verifies the registration, saves the tools in its `DynamicToolRegistry`, maps each tool name to the socket descriptor, and returns an acknowledgment:
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "status": "registered",
    "registered_tools": 2
  }
}
```

Immediately following registration, `aimon` emits an MCP notification across active SSE sessions to inform AI assistants that the tool catalog has expanded:
```json
{
  "jsonrpc": "2.0",
  "method": "notifications/tools/list_changed",
  "params": {}
}
```

### 3.2 Tool Invocation Proxying (`tools/call`)
When an AI assistant issues a `tools/call` for a dynamically registered tool:
1. `McpServer` looks up the tool in the `DynamicToolRegistry`.
2. `TcpGateway` forwards the request verbatim to the originating socket:
   ```json
   {
     "jsonrpc": "2.0",
     "id": "aimon-req-101",
     "method": "tools/call",
     "params": {
       "name": "meshmon_get_node_status",
       "arguments": {"node_id": "!1234abcd"}
     }
   }
   ```
3. The satellite daemon executes the command against its internal subsystems (e.g. querying `MeshMonDb` or Zyxel SSH) and sends back the result:
   ```json
   {
     "jsonrpc": "2.0",
     "id": "aimon-req-101",
     "result": {
       "content": [
         {
           "type": "text",
           "text": "### Node !1234abcd (RoofRepeater)\n- **Battery**: 98%\n- **SNR**: +8.5 dB\n- **Hops**: 1"
         }
       ]
     }
   }
   ```
4. `aimon` relays the response directly back to the AI assistant over SSE.

### 3.3 Heartbeats & Failure Handling
- **TCP Keepalive & Pings**: `aimon` periodically sends `ping` requests over idle connections.
- **Graceful Disconnect / Crash Handling**: If a satellite socket closes:
  - `aimon` removes all tools associated with that connection.
  - Emits `notifications/tools/list_changed` to connected AI agents.
  - If a call is in-flight when disconnection occurs, `aimon` responds with JSON-RPC error code `-32000` (`"Subsystem disconnected during tool execution"`).

### 3.4 Dynamic Toolset Evolution & Non-Disruptive Updates
When developers implement new features in downstream daemons (such as `meshmon` or `netmon`), those capabilities must be made available to AI assistants without shutting down `aimon` or restarting the AI IDE.

This zero-downtime evolution is achieved through a two-tier dynamic registration protocol:
1. **Runtime Tool Updates (`gateway/updateTools`)**:
   The TCP socket between satellite daemons and `aimon` is long-lived and bidirectional. When a daemon wants to expose a new tool or modify schemas, it issues `gateway/updateTools` over the active connection without dropping the link:
   ```json
   {
     "jsonrpc": "2.0",
     "method": "gateway/updateTools",
     "params": {
       "subsystem": "meshmon",
       "tools": [
         /* Full updated list of tool schemas */
       ]
     }
   }
   ```
   `aimon`'s in-memory `DynamicToolRegistry` immediately updates the active schema mappings.
2. **Native MCP Push to AI Clients (`notifications/tools/list_changed`)**:
   Immediately upon updating its internal catalog, `aimon` broadcasts the standard MCP notification across all connected SSE streams to AI agents:
   ```json
   {"jsonrpc": "2.0", "method": "notifications/tools/list_changed", "params": {}}
   ```
   AI assistants (Antigravity and Cursor) automatically query `tools/list` in the background and update their available tool palette. The developer and AI agent gain access to the new capabilities instantly—with **zero downtime**, **zero IDE restarts**, and **zero interruption** to ongoing chat sessions.
3. **Independent Daemon Recompilation**:
   If a daemon binary is recompiled and restarted (e.g. inside its dedicated `screen` session), only that specific satellite process restarts. `aimon` and the IDE remain running. The restarted daemon automatically reconnects to `aimon:3885` and re-registers its tools within seconds.

### 3.5 Fault Tolerance, Process Failures & Graceful Auto-Recovery

The architecture implements a self-healing, loosely coupled state machine designed for distributed heterogeneous hardware:

```text
┌────────────────────────────────────────────────────────────────────────────────┐
│                           Process Failure Lifecycle                            │
└────────────────────────────────────────────────────────────────────────────────┘

1. Satellite Drops (e.g. meshmon satellite restarts or reboots):
   - aimon detects socket closure (read EOF / ECONNRESET).
   - aimon unbinds meshmon_* tools from DynamicToolRegistry.
   - aimon pushes notifications/tools/list_changed to IDE.
   - In-flight calls cleanly return: "Subsystem 'meshmon' is currently offline."

2. Satellite Autonomous Reconnect (AimonGatewayClient on satellite):
   - Background worker detects disconnect, enters exponential backoff (1s, 2s, 4s... max 30s).
   - Local radio ingestion, SQLite logging, and MQTT keep running unaffected.
   - Once network or process is restored, opens TCP to <gateway-host>:3885 and sends gateway/register.

3. Gateway Re-registration:
   - aimon binds the tools and pushes notifications/tools/list_changed to AI.
   - Fully recovered with zero human or AI intervention.
```

#### Detailed Failure Scenarios:

#### Scenario A: Satellite Daemon (`meshmon` or `netmon`) Crashes or Reboots
1. **Clean Tool Withdrawal**: `aimon` detects socket closure immediately, removes all tools belonging to that connection from `DynamicToolRegistry`, and notifies connected AI assistants via `notifications/tools/list_changed`.
2. **Graceful In-Flight Error Handling**: If an AI assistant invokes a tool at the exact moment a satellite goes down, `aimon` intercepts the request and responds with a clean, structured JSON-RPC error:
   ```json
   {
     "jsonrpc": "2.0",
     "id": 42,
     "error": {
       "code": -32000,
       "message": "Subsystem 'meshmon' disconnected during tool execution."
     }
   }
   ```
   The AI agent receives this cleanly and can report: *"The meshmon service appears to be restarting, please try again shortly"*, avoiding an unhandled IDE crash or hanging request.
3. **Autonomous Auto-Reconnect**: Satellite daemons run an autonomous connection supervisor thread with exponential backoff (1s, 2s, 4s, up to 30s). Once the daemon or network recovers, it re-establishes the TCP connection, sends `gateway/register`, and `aimon` restores the tools to the AI assistant.

#### Scenario B: Gateway (`aimon`) Restarts or Reboots
1. **Loosely Coupled Independent Operation**: Downstream daemons (`meshmon` and `netmon`) are completely decoupled from `aimon` for their primary operational tasks:
   - `meshmon` continues receiving radio packets, writing to SQLite `MeshMonDb`, and publishing MQTT to Home Assistant.
   - `netmon` continues recording packet traces, running the LAN sniffer, and monitoring SNMP.
   - *Neither satellite crashes or stalls when `aimon` is offline.*
2. **Automatic Reconnection**: Satellite supervisor threads detect the broken TCP socket and continuously retry connecting in the background. The moment `aimon` restarts and binds port `3885`, satellites reconnect and re-export their tools within seconds.
3. **IDE Stream Resiliency**: AI assistants (Antigravity and Cursor) feature built-in HTTP SSE reconnection logic, automatically reconnecting to `http://<host>:3883/sse` when `aimon` returns.

#### Summary of Resilience Matrix:

| Event | System Behavior | Impact on Developer / IDE |
| :--- | :--- | :--- |
| **Add/Modify Tool in Satellite** | Satellite sends `gateway/updateTools` or restarts | **Zero disruption** to `aimon` or IDE |
| **Satellite Crashes / Reboots** | `aimon` unbinds tools, returns clean error to in-flight calls; satellite auto-reconnects | **Self-healing**, no IDE restart required |
| **Gateway (`aimon`) Restarts** | Satellites continue local tasks, auto-reconnect when `aimon` is back | **Self-healing**, satellites unaffected |

---

## 4. Multi-Node Deployment Topology

In production environments, `aimon` and its satellite daemons typically deploy across dedicated machines:

| Node Type | Role | Connectivity to aimon |
| :--- | :--- | :--- |
| **Gateway Host** | Runs `aimon`, exposes MCP over SSE (`:3883`), listens on TCP Gateway (`:3885`). | Local / Loopback |
| **Radio Gateway Node** | Runs `meshmon`, physically connected to LoRa radios via USB/serial. | Outbound TCP to Gateway `:3885` |
| **Network Monitor Node** | Runs `netmon`, situated on core LAN switch mirror port for promiscuous capture. | Outbound TCP to Gateway `:3885` |

### Process Lifecycle & Persistent Sessions
All services typically run as systemd daemons or inside persistent terminal multiplexers (such as GNU `screen` or `tmux`) so human operators can attach for real-time diagnostic output while AI agents query the unified MCP interface.

---

## 5. AI Toolset Matrix: Quota Analytics & Gateway Telemetry

`aimon` exports a focused suite of native MCP tools designed for real-time developer quota awareness and hardware/network telemetry proxying:

| Tool Name | Operation Mode | Utility Description |
| :--- | :--- | :--- |
| `check_antigravity_quota` | **Analytics** | Queries remaining 5-hour rolling capacity %, prompt/flow credits, model tiers, and reset countdown timestamps for Google Antigravity / Gemini models. |
| `check_cursor_usage` | **Analytics** | Queries fast requests used vs plan limit, total billing spend ($), and monthly billing cycle reset date for Cursor. |
| `get_combined_ai_status` | **Analytics** | Formats an executive summary contrasting both Google Antigravity and Cursor subscriptions in a single Markdown card. |

### Satellite-Proxied Toolsets
Connected subsystem daemons dynamically export their domain-specific MCP tools through `aimon`:
- **`embdevenv`** (Embedded Hardware): `embdevenv_mcu_*`, `embdevenv_console_*`, `embdevenv_flash_*`, `embdevenv_list_targets`, `embdevenv_get_target_status` (16 tools).
- **`netmon`** (Network Security & Sniffer): `firewall_*`, `lan_*`, `snmp_*` (13 tools).
- **`meshmon`** (LoRa Radio Mesh): `meshmon_get_node_status`, `meshmon_get_rf_analytics`, `meshmon_query_db`, `meshmon_send_message`, etc. (6 tools).

---

## 6. Scoped MCP Tool Profiling & Auto-Scoping

To prevent agent context pollution and minimize token consumption across diverse software workspaces, `aimon` provides compile-time and runtime profile filtering:

| Profile | Purpose / Domain | Included Tools |
| :--- | :--- | :--- |
| `core` | Default / general programming | `check_antigravity_quota`, `check_cursor_usage`, `get_combined_ai_status` |
| `embedded` | Target boardbringup (`boards`, `embdevenv`) | Core tools + all `embdevenv_*` tools (19 tools total) |
| `network` | Network diagnostics & firewall (`netmon`, `network`) | Core tools + `firewall_*`, `lan_*`, `snmp_*` |
| `mesh` | LoRa RF telemetry (`meshmon`) | Core tools + `meshmon_*` tools |
| `all` | Full gateway administrator | Complete catalog across all connected satellites |

### Profile Selection Mechanisms
1. **Automatic Workspace Detection**: When an AI agent connects via MCP `initialize`, `aimon` inspects `workspaceFolders` or `rootUri`. If the workspace matches a known hardware or network domain (e.g. `boards`, `embdevenv`, `netmon`, `meshmon`), `aimon` automatically scopes the session.
2. **Explicit URL Query Parameter**: Clients connecting via SSE specify `http://<host>:3883/sse?profile=<profile>`.
3. **HTTP Header**: Clients pass `X-Aimon-Profile: <profile>`.
4. **CLI Stdio Mode**: When spawned as a pipe process, pass `--profile=<profile>`.

Tools invoked outside the active profile are rejected with JSON-RPC error code `-32601` (`Tool '<name>' is not available under active profile '<profile>'`).

---

## 7. Strict MCP Primacy & Direct API Access Control

All AI agents in the ecosystem must interact with `aimon` exclusively through the official Model Context Protocol (JSON-RPC 2.0).
- Direct REST/HTTP endpoint queries (`/api/*`) are restricted and return HTTP `403 Forbidden` for non-dashboard clients.
- Autonomous scripts or agents must never bypass MCP by calling raw socket, Telnet, or REST interfaces.
- When an MCP tool call fails or times out, agents must adhere to the **Strict Stop-and-Report** protocol without attempting side-channel workarounds.

---

## 8. Rolling Agent Telemetry Database Subsystem (`AgentTelemetryDb`)

To provide deep observability into agent cognitive cycles, execution durations, and resource consumption without relying on third-party cloud analytics, `aimon` includes an integrated, local-first rolling time-series engine modeled after the proven `SnmpDatabase` architecture in `netmon`.

### 8.1 SQLite WAL Schema & Storage Engine
`AgentTelemetryDb` stores data in SQLite with Write-Ahead Logging (`WAL`) enabled, dynamic downsampling, and automated retention pruning:

```sql
-- Active and historical agent execution sessions
CREATE TABLE IF NOT EXISTS agent_sessions (
    session_id          TEXT PRIMARY KEY,
    conversation_id     TEXT,
    agent_type          TEXT NOT NULL,
    workspace           TEXT,
    model               TEXT,
    start_timestamp     INTEGER NOT NULL,
    end_timestamp       INTEGER,
    status              TEXT DEFAULT 'RUNNING',
    total_turns         INTEGER DEFAULT 0,
    prompt_tokens       INTEGER DEFAULT 0,
    comp_tokens         INTEGER DEFAULT 0,
    tool_calls          INTEGER DEFAULT 0,
    errors              INTEGER DEFAULT 0,
    avg_turn_ms         REAL DEFAULT 0.0
);

-- Granular per-step lifecycle and tool invocation events
CREATE TABLE IF NOT EXISTS agent_lifecycle_events (
    id                  INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp           INTEGER NOT NULL,
    session_id          TEXT NOT NULL,
    agent_type          TEXT NOT NULL,
    event_type          TEXT NOT NULL,
    step_index          INTEGER DEFAULT 0,
    tool_name           TEXT,
    duration_ms         REAL DEFAULT 0.0,
    status              TEXT DEFAULT 'OK',
    details_json        TEXT,
    FOREIGN KEY(session_id) REFERENCES agent_sessions(session_id)
);

-- High-frequency telemetry samples (14-day retention)
CREATE TABLE IF NOT EXISTS agent_telemetry_samples (
    timestamp           INTEGER PRIMARY KEY,
    active_agents       INTEGER DEFAULT 0,
    prompt_tokens_sec   REAL DEFAULT 0.0,
    comp_tokens_sec     REAL DEFAULT 0.0,
    tool_calls_sec      REAL DEFAULT 0.0,
    error_rate_pct      REAL DEFAULT 0.0,
    avg_turn_latency_ms REAL DEFAULT 0.0,
    p95_turn_latency_ms REAL DEFAULT 0.0,
    approval_wait_ms    REAL DEFAULT 0.0
);

-- Aggregated hourly rollups (365-day retention)
CREATE TABLE IF NOT EXISTS agent_hourly_rollups (
    hour_bucket         INTEGER PRIMARY KEY,
    total_prompt_tokens INTEGER DEFAULT 0,
    total_comp_tokens   INTEGER DEFAULT 0,
    total_tool_calls    INTEGER DEFAULT 0,
    total_errors        INTEGER DEFAULT 0,
    avg_turn_latency_ms REAL DEFAULT 0.0,
    p95_turn_latency_ms REAL DEFAULT 0.0,
    avg_approval_wait_ms REAL DEFAULT 0.0
);
```

### 8.2 Dynamic Downsampling Engine
When the web frontend or mobile app queries timeseries data over wide windows (`7d`, `30d`, `1y`), `AgentTelemetryDb::queryTimeseries(window, maxPoints)` dynamically downsamples the dataset into evenly spaced buckets to guarantee sub-millisecond query execution and bounded payload sizes.

---

## 9. Mobile Companion Action Approval Latching Subsystem (`MobileGateway`)

Autonomous agent tasks frequently stall when sensitive tools (`run_command`, `write_to_file`, `multi_replace_file_content`, `embdevenv_mcu_power`) request human confirmation. `aimon` provides an asynchronous action approval latching engine that delivers real-time confirmation requests directly to the developer's mobile device lock screen.

### 9.1 Latching Protocol & Concurrency Architecture
1. **Hook Interception**: When Antigravity or Cursor triggers a sensitive tool, `scripts/mobile_permission_relay.py` intercepts the JSON invocation and executes a synchronous `POST /api/approvals/request` to `aimon`.
2. **Promise Latch Creation**: `MobileGateway::requestApproval()` registers a pending action record with a unique UUID and initializes a `std::promise<ApprovalVerdict>`. The calling HTTP worker thread blocks synchronously on `std::future<ApprovalVerdict>::get()`.
3. **Multicast Notification**: `MobileGateway` broadcasts an SSE / WebSocket approval notification to paired Android devices.
4. **Lock-Screen Verdict**: The operator taps `[Approve]` or `[Deny]` directly from the Android heads-up notification. The mobile app posts the verdict back to `/api/approvals/decision`.
5. **Latch Resolution**: The promise resolves instantly in C++, unblocking the HTTP worker thread and returning the approval decision to the IDE hook.
6. **Reaper Fallback**: If no mobile response is received within 120 seconds, a background timeout reaper thread automatically expires the request and fails safely.

### 9.2 Cryptographic Authentication & Replay Defense
- **128-bit Pairing Secrets**: Pairing generates a high-entropy 128-bit cryptographic secret formatted for QR code scanning or manual entry.
- **Single-Use Action Tokens**: Each notification payload includes a single-use action token. Once a decision is submitted, the token is permanently invalidated to prevent replay attacks.
- **Instant Revocation**: Devices can be audited and revoked individually from the web dashboard or CLI.

---

## 10. Web Analytics & Multi-Surface Approvals Dashboard

The `aimon` dashboard (`web/index.html`, `web/app.js`, `web/style.css`, `include/WebAssets.hxx`) provides a multi-tab single-page interface:

1. **AI Quotas (`#view-aimon`)**: Untouched front page displaying Google Antigravity quotas, credit allowances, and Cursor Fast Request usage.
2. **Agent Telemetry & Analytics (`#view-telemetry`)**: Interactive SVG/Canvas charts for Token Velocity, Turn Latency & P95 Distribution, Tool Invocation Matrices, and Session Waterfalls with timeframe selectors (`1H`, `24H`, `7D`, `30D`, `1Y`).
3. **Action Approvals (`#view-approvals`)**: Live pending approval actions, 128-bit mobile pairing QR interface, and paired device management.
4. **Dynamic Satellite Panels (`#panel-<id>`)**: Seamless embedded iframe monitors for connected subsystems (`netmon`, `meshmon`, `embdevenv`).

---

## 11. Dual-IDE Universal Hook Relays

`scripts/mobile_permission_relay.py` standardizes tool interception across both IDEs:
- **Google Antigravity**: Configured via `.agents/hooks.json` (`PreToolUse`, `PostToolUse`).
- **Cursor IDE**: Configured via `.cursor/hooks.json` (`preTool`, `postTool`).

---

## 12. Android Companion Application Architecture

The Android companion application (`com.selfso.aimon`) is built using Kotlin and Jetpack Compose:
- **`AimonWebSocketClient`**: OkHttp client with automatic exponential backoff reconnection and 15s heartbeats.
- **`AimonNotificationManager`**: Native Android `NotificationCompat` builder creating high-priority heads-up notifications with embedded `[Approve]` and `[Deny]` action buttons.
- **Glance AppWidget**: Android home-screen widget displaying real-time AI quota balances and active agent session counts.

---

## License & Copyright

Copyright (C) 2026, Charles Chiou. All rights reserved.
