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
3. **Multiplexed TCP Gateway for Subsystems**: Runs a TCP gateway server on port `3885` accepting connections from specialized network daemons (`meshmon` on `fox`, `netmon` on `rhino`). When daemons connect, they dynamically register their toolsets; `aimon` merges these tools into its global MCP registry and proxies RPC invocations transparently.
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
│   │  - check_cursor_usage             │  │  - Dynamic RPC routing table │   │
│   │  - get_combined_ai_status         │  │  - Tool state notifications  │   │
│   │  - register_agent_task            │  │  - Lifecycle / heartbeats    │   │
│   │  - list_active_tasks              │  │                              │   │
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
    "hostname": "fox",
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

1. Satellite Drops (e.g. meshmon on fox restarts or reboots):
   - aimon detects socket closure (read EOF / ECONNRESET).
   - aimon unbinds meshmon_* tools from DynamicToolRegistry.
   - aimon pushes notifications/tools/list_changed to IDE.
   - In-flight calls cleanly return: "Subsystem 'meshmon' is currently offline."

2. Satellite Autonomous Reconnect (AimonGatewayClient on fox):
   - Background worker detects disconnect, enters exponential backoff (1s, 2s, 4s... max 30s).
   - Local radio ingestion, SQLite logging, and MQTT keep running unaffected.
   - Once network or process is restored, opens TCP to builder:3885 and sends gateway/register.

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

## 5. AI Toolset Matrix: Analytics, Management & Workflow Automation

`aimon` exports a focused suite of native MCP tools designed for real-time developer quota awareness and multi-agent coordination:

| Tool Name | Operation Mode | Utility Description |
| :--- | :--- | :--- |
| `check_antigravity_quota` | **Analytics** | Queries remaining 5-hour rolling capacity %, prompt/flow credits, model tiers, and reset countdown timestamps for Google Antigravity / Gemini models. |
| `check_cursor_usage` | **Analytics** | Queries fast requests used vs plan limit, total billing spend ($), and monthly billing cycle reset date for Cursor. |
| `get_combined_ai_status` | **Analytics** | Formats an executive summary contrasting both Google Antigravity and Cursor subscriptions in a single Markdown card. |
| `register_agent_task` | **Management & Workflow** | Autonomous agents register their current high-level goal, active file/step, and status to prevent duplicate work and inform the web dashboard. |
| `list_active_tasks` | **Workflow Automation** | Inspects active agent fleet tasks across local and remote sessions, enabling multi-agent coordination and status monitoring. |

### Practical Agent Usage Scenarios
- **Analytics**:
  - *"Do I have enough Cursor fast requests remaining to do a large codebase refactor, or should I wait for tomorrow's billing cycle reset?"*
  - *"Check my Gemini 3.6 capacity and tell me when the 5-hour rolling bucket refreshes."*
- **Workflow Automation & Self-Throttling**:
  - Autonomous agents can call `check_antigravity_quota` before beginning a high-volume task; if capacity is below 10%, the agent can self-throttle or choose a lighter model tier.
  - During complex multi-file migrations, agents call `register_agent_task` at each milestone, allowing human developers and peer agents to track live progress on the `aimon` web dashboard (`localhost:3883`).
