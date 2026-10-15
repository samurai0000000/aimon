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
| `check_cursor_usage` | **Analytics** | Queries fast requests used vs plan limit, total billing spend ($), per-category spend breakdown, and monthly billing cycle reset date for Cursor. |
| `check_claude_usage` | **Analytics** | Estimated Claude Code tokens and cost per account over 5 hours, 7 days, the configured window and the billing cycle, per-model table, plan tier, and price freshness. Read from local transcripts; figures are estimates. |
| `get_combined_ai_status` | **Analytics** | Formats an executive summary of Google Antigravity, Cursor and Claude Code in a single Markdown card. |
| `get_usage_insights` | **Coaching & Analytics** | Returns programmatic and workflow-coaching findings across Claude Code, Cursor, and Antigravity with severity, confidence, sample size (`n`), and suggested experiments. |
| `get_usage_report` | **Analytics** | Structured cross-platform token, cache-hit, sidechain, and cost breakdown grouped by `project`, `model`, `day`, `hour_of_day`, `tool`, or `session`. |
| `export_usage_dataset` | **Analytics** | Exports the normalized `insights.db` tables (`sqlite`, `csv`, or `jsonl` with `0600` permissions and optional `--anonymize` project hashing) for offline or agent-driven SQL analysis. |
| `describe_usage_dataset` | **Analytics** | Returns the `insights.db` table schemas, privacy guarantees, and sample SQL queries. |
| `compare_experiment` | **Coaching** | Compares a detector's target metric before and after a `split_epoch` timestamp to evaluate a workflow-coaching experiment (`improved`, `regressed`, `unchanged`, or `insufficient_data`). |

### 5.1 Claude Code Usage Subsystem

**Data path.** `ClaudeCollector` runs in the daemon's poll cycle. For each configured account it walks `<config dir>/projects/**/*.jsonl` (subagent transcripts live in `<session>/subagents/` and are covered by the same walk), reads only bytes added since the last poll (per-file cursors persisted in SQLite, partial trailing lines deferred, 4 MiB line cap, shrunk or replaced files re-read), and turns assistant lines carrying `usage` into rows `(account, message id, request id, time, model, speed, input, output, cache read, cache write 5m, cache write 1h, web searches)`. No prompt text, working directory or identity is extracted. `ClaudeUsageStore` upserts rows keyed by `(account, message id, request id)`, keeping the row with the strictly greatest output tokens (streamed duplicates), and prunes rows older than `retention_days`.

**Costing.** All amounts are integer nano-dollars per token (`$P / MTok` is `P x 1000`), summed exactly; there is no floating point in the cost path. `PriceCatalog` fetches the markdown form of Anthropic's pricing page, parses the model and fast-mode tables strictly (exact headings and column headers, every row validated: positive prices, cache writes 1.25x and 2x of input, cache read not above input, at most 3 decimals), caches it atomically with mode `0600`, and refreshes it off the poll thread (back-off after a failure). Structural problems reject the page; a problem in one model's row quarantines only that model, which stays unpriced. A model priced by prompt length (two rows, `up to` and `over` N tokens) uses the upper rate set when `input + cache read + cache writes` exceeds N. Each fetched page with a new hash is stored as a **price version**; a message is costed with the latest version fetched at or before its time (older messages use the first version), so past days do not change when prices do. Fast mode uses the fast rates and is unpriced for models without them.

**Tier and limits.** The tier comes only from two descriptive strings of `<config dir>/.credentials.json` (`subscriptionType`, `rateLimitTier`); the file must be mode `0600`, is capped at 64 KiB, is never written, and token values never leave the parsing buffer. `enterprise` maps to Enterprise (dollar cycle with optional `spend_limit_usd`); any other value is Personal (dollars labelled API-equivalent value, no budget); anything unreadable is Unknown while usage is still collected.

**Surfaces.** `check_claude_usage` (in every profile), the combined status, history samples `claude:<account>` (`est_cost_usd`, `total_tokens`), MQTT/Home Assistant sensors per account, the `claude` section of the status JSON, and a static dashboard card (no JavaScript-generated cards). Every dollar figure is labelled an estimate: it can differ from the billing page (price changes, other machines, other products).

**Verification.** Totals are checked against an independent Python recount that re-parses the pricing page and the transcripts with its own code and the same integer arithmetic, including every time window, per model and per day.

### 5.2 Usage Insights & Workflow Coaching Subsystem (`InsightEngine`)

`InsightEngine` maintains a dedicated SQLite WAL database (`~/.config/aimon/insights.db`, mode `0600`) that normalizes cross-platform usage metadata across Claude Code, Cursor, and Google Antigravity without storing any prompt text, assistant text, thinking text, or raw file paths.

- **Normalized Schema (`insights.db`)**:
  - `ins_requests`: Per-request token, cache (`cache_read`, `cache_write_5m`, `cache_write_1h`), `thinking_tokens`, `context_tokens`, `compacted`, `is_sidechain`, and `est_cost_nano` records.
  - `ins_sessions`: Session rollups (`duration_sec`, `active_sec`, `turn_count`, `request_count`, `tool_calls`, `tool_errors`, `edit_count`, `unique_files`, `peak_context`, `compaction_count`, `cost_basis`).
  - `ins_daily`: Daily rollups by platform, day, and model.
  - `ins_tool_calls`: Tool executions with 16-hex-char SHA-256 `target_hash` (never raw file paths).
  - `ins_quota`: Rolling quota fraction and credit balance snapshots across platforms.
  - `ins_spend_daily`: Vendor-reported daily spend in cents by category (`CursorCollector::fetchDailySpendByCategory`).
  - `ins_generations`: Validated numeric records decoded from Antigravity `gen_metadata` protobuf blobs (`input_tokens`, `output_tokens`, `thinking_tokens`, `response_tokens`, `cache_read`, `ttft_ms`, `context_tokens`).
  - `ins_prompts`: Numeric prompt structure features (`chars`, `words`, `lines`, `prose_chars`, `prose_words`, `fenced_blocks`, `fenced_chars`, `paste_chars`, `mixed_paste`, `file_refs`, `question_marks`, `has_imperative`, `is_first_in_session`) paired with outcome proxies (`next_tool_calls`, `next_tool_errors`, `next_edits`, `reprompt_within_120s`, `interrupted_turn`, `turn_tokens`).
  - `ins_findings`: Programmatic and workflow-coaching detector findings (`cache_efficiency`, `subagent_share`, `delegation_style`, `long_context`, `compact_opportunity`, `long_running_sessions`, `session_sprawl`, `error_retry_loops`, `short_prompt_rate`, `first_prompt_specificity`, `drip_vs_batch`, `time_and_fatigue`, `spend_category_mix`, `cost_by_project`, `model_mix`, `model_choice_fit`, `idle_gap_cache_expiry`, `context_reset_habit`, `time_of_day`, `cross_platform_overlap`, `quota_pressure`, `burn_rate_forecast`).
  - `ins_scan_cursors`: Independent per-file scan cursors (`claude_jsonl`, `ag_conv_db`, `ag_transcript`) so historical transcripts already at EOF in `claude_file_cursors` are backfilled from offset 0.

### 5.3 EDA & Robotics CAD Bridge & Local CPU Accelerators (`EdaBridge` / `EdaAccelerators` / `aimon-eda-bridge`)

`aimon` provides a 100% native C++17 EDA and Robotics CAD subsystem (`EdaBridge` + `EdaAccelerators` + `EasyEdaConverter` + `KiCadSchematicEditor`) paired with the cross-platform `aimon-eda-bridge` daemon (`tools/eda-bridge/`) and companion desktop GUI plugins (`tools/eda-bridge/plugins/`). Together they expose **36 MCP tools** under the `eda` (and `all`) profile:

1. **Always-Available Local C++17 CPU Algorithmic Accelerators (`EdaAccelerators` — 7 Tools)**:
   Hosted directly in-process inside `aimon` so AI agents deterministically compute layout, routing, circuit synthesis, kinematics, and mechanisms in milliseconds without guessing math in tokens:
   - `eda_calc_schematic_layout`: Sugiyama layered schematic placement + 50-mil (`1.27 mm`) A\* orthogonal wire router (`symbols[]`, `wires[]`).
   - `eda_calc_pcb_placement`: Simulated annealing + force-directed Half-Perimeter Wire Length (`HPWL`) floorplanner with zero courtyard overlap and decoupling-capacitor clustering (`placements[]`).
   - `eda_calc_pcb_autoroute`: PathFinder negotiated-congestion multi-layer 45-degree A\* trace and via router (`tracks[]`, `vias[]`).
   - `eda_calc_electrical_synth`: E24/E96 & JLCPCB-Basic passive divider/filter solver, IPC-2152 trace/via sizing, microstrip impedance calculator, and power/thermal budget analyzer.
   - `cad_calc_robot_kinematics_statics`: DH/URDF forward/inverse kinematics, gravity + payload joint torque budget, servo safety factor, and whole-body CoM support-polygon margin.
   - `cad_calc_mechanism_synth`: Analytical involute spur/helical gears, planetary gearsets ($Z_s + 2Z_p = Z_r$), GT2/HTD timing pulleys, 25T/24T servo splines, and Grashof 4-bar linkages with FreeCAD CSG code generation.
   - `cad_calc_print_orientation_dfm`: Spherical FDM build-orientation optimizer and anisotropic Z-layer delamination safety factor analyzer for `PLA`, `PETG`, `ABS`, `ASA`, and `PA-CF`.
2. **KiCad Schematic, PCB & Manufacturing Tools (9 Tools)**:
   Backed by `KiCadSchematicEditor` (C++17 S-expression engine), companion `pcbnew` plugin (`tools/eda-bridge/plugins/kicad-aimon-bridge/`), and `kicad-cli`: `kicad_search_library`, `kicad_inspect_project`, `kicad_edit_schematic`, `kicad_sync_sch_to_pcb`, `kicad_edit_pcb`, `kicad_run_erc`, `kicad_run_drc`, `kicad_export_fab_pack`, and `kicad_export_step`.
3. **FreeCAD Robotics & 3D-Printing Tools (9 Tools)**:
   Backed by `tools/eda-bridge/plugins/freecad-aimon-bridge/` and headless `FreeCADCmd` fallback, guarded by `EdaBridge::guardFreecadScript()` (rejecting brittle `Face1`/`Edge1`/`Vertex1` TNP references and OS/network escape imports): `freecad_live_status`, `freecad_inspect_document`, `freecad_generate_cad`, `freecad_edit_parameters`, `freecad_assemble_joint`, `freecad_analyze_printability`, `freecad_render_view`, `freecad_export_print_files`, and `freecad_invoke_api`.
4. **EasyEDA Pro & Conversion Tools (11 Tools)**:
   Backed by the packaged `.eext` extension (`tools/eda-bridge/plugins/easyeda-pro-extension/`) over `AppPluginRelay` (`127.0.0.1:3893`) and C++17 `EasyEdaConverter`: `easyeda_live_status`, `easyeda_search_library`, `easyeda_inspect_schematic`, `easyeda_edit_schematic`, `easyeda_sync_sch_to_pcb`, `easyeda_inspect_pcb`, `easyeda_edit_pcb`, `easyeda_run_drc`, `easyeda_export_jlcpcb`, `easyeda_invoke_api`, and `easyeda_convert`.
5. **Bidirectional Transport, Sandboxed Path Translation & Safety**:
   - Supports outward TCP JSON-RPC (`aimon` connects to `aimon-eda-bridge` on port `3892`) and inward `TcpGateway` registration (`aimon-eda-bridge --connect <aimon-host>:3885` registers as `subsystem: "eda"`), authenticated via shared secret (`authSecret` / `AIMON_EDA_AUTH_SECRET`).
   - `EdaBridge` bidirectionally rewrites Linux workspace paths (`linux_path_prefix`, default `~/work`) and Windows drive paths (`windows_path_prefix`, default `Z:\`), rejecting any `..` traversal or out-of-workspace path with `-32602`.

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
| `core` | Default / general programming | Quota tools (`check_antigravity_quota`, `check_cursor_usage`, `check_claude_usage`, `get_combined_ai_status`) and Insights tools (`get_usage_insights`, `get_usage_report`, `export_usage_dataset`, `describe_usage_dataset`, `compare_experiment`) |
| `eda` | PCB design & 3D robotics CAD (`kicad`, `freecad`, `easyeda`, `pcb`, `cad`, `robot`, `hw-`) | Core tools + all 36 EDA/CAD & CPU accelerator tools (`eda_calc_*`, `cad_calc_*`, `kicad_*`, `freecad_*`, `easyeda_*`) |
| `embedded` | Target board bringup (`boards`, `embdevenv`) | Core tools + all `embdevenv_*` tools |
| `network` | Network diagnostics & firewall (`netmon`, `network`) | Core tools + `firewall_*`, `lan_*`, `snmp_*` |
| `mesh` | LoRa RF telemetry (`meshmon`) | Core tools + `meshmon_*` tools |
| `mail` | Mail access via `alpine-mcp` | Core tools + `mail_*` tools |
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

### 8.0 Event Sources

Events arrive on `POST /api/telemetry/event` from lifecycle hooks. `agent_type` is free text; the dashboard knows three values: `antigravity`, `cursor` and `claude` (any other value is grouped with Antigravity). The concurrency chart (`queryActivityTimeline`) keeps one series per type: `antigravity_active`, `cursor_active` and `claude_active`. Claude Code hooks send no token counts; its tokens and estimated cost come from the Claude usage collector instead (see Setup Step 6.5).

### 8.1 SQLite WAL Schema & Storage Engine
`AgentTelemetryDb` stores data in SQLite with Write-Ahead Logging (`WAL`) enabled, dynamic downsampling, and automated retention pruning:

```sql
-- Active and historical agent execution sessions
CREATE TABLE IF NOT EXISTS agent_sessions (
    session_id          TEXT PRIMARY KEY,
    conversation_id     TEXT NOT NULL,
    agent_type          TEXT NOT NULL,
    workspace_path      TEXT NOT NULL,
    model_name          TEXT NOT NULL,
    start_timestamp     INTEGER NOT NULL,
    end_timestamp       INTEGER DEFAULT 0,
    status              TEXT NOT NULL,
    total_turns         INTEGER DEFAULT 0,
    total_prompt_tokens INTEGER DEFAULT 0,
    total_comp_tokens   INTEGER DEFAULT 0,
    total_tool_calls    INTEGER DEFAULT 0,
    total_errors        INTEGER DEFAULT 0,
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
    tool_name           TEXT DEFAULT '',
    duration_ms         REAL DEFAULT 0.0,
    status              TEXT DEFAULT 'OK',
    details_json        TEXT DEFAULT '{}'
);

-- High-frequency telemetry samples (14-day retention)
CREATE TABLE IF NOT EXISTS agent_telemetry_samples (
    timestamp           INTEGER NOT NULL,
    active_agents       INTEGER NOT NULL,
    prompt_tokens_sec   REAL NOT NULL,
    comp_tokens_sec     REAL NOT NULL,
    tool_calls_sec      REAL NOT NULL,
    error_rate_pct      REAL NOT NULL,
    avg_turn_latency_ms REAL NOT NULL,
    p95_turn_latency_ms REAL NOT NULL,
    approval_wait_ms    REAL NOT NULL
);

-- Aggregated hourly rollups (365-day retention)
CREATE TABLE IF NOT EXISTS agent_hourly_rollups (
    hour_timestamp       INTEGER NOT NULL,
    agent_type           TEXT NOT NULL,
    model_name           TEXT NOT NULL,
    total_sessions       INTEGER NOT NULL,
    total_turns          INTEGER NOT NULL,
    total_prompt_tokens  INTEGER NOT NULL,
    total_comp_tokens    INTEGER NOT NULL,
    total_tool_calls     INTEGER NOT NULL,
    total_errors         INTEGER NOT NULL,
    avg_turn_ms          REAL NOT NULL,
    p95_turn_ms          REAL NOT NULL,
    max_turn_ms          REAL NOT NULL,
    avg_approval_wait_ms REAL NOT NULL,
    PRIMARY KEY (hour_timestamp, agent_type, model_name)
);
```

### 8.2 Dynamic Downsampling Engine
When the web frontend or mobile app queries timeseries data over wide windows (`7d`, `30d`, `1y`), `AgentTelemetryDb::queryTimeseries(window, maxPoints)` dynamically downsamples the dataset into evenly spaced buckets to guarantee sub-millisecond query execution and bounded payload sizes.

---

## 9. Mobile Companion Action Approval Latching Subsystem (`MobileGateway`)

Autonomous agent tasks frequently stall when sensitive tools (`run_command`, `write_to_file`, `multi_replace_file_content`, `embdevenv_mcu_power`) request human confirmation. `aimon` provides an asynchronous action approval latching engine that delivers real-time confirmation requests directly to the developer's mobile device lock screen.

### 9.1 Latching Protocol & Concurrency Architecture
1. **Hook Interception**: When Antigravity or Cursor triggers a sensitive tool, the configured IDE hook interceptor intercepts the JSON invocation and executes a synchronous `POST /api/approvals/request` to `aimon`.
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

1. **AI Quotas (`#view-aimon` / `#subpanel-quotas`)**: Front page displaying Google Antigravity quotas and credit allowances, Cursor Fast Request and per-category spend breakdown, and Claude Code estimated usage.
2. **Agent Telemetry & Analytics (`#subpanel-telemetry`)**: Fed by lifecycle hooks from Antigravity, Cursor and Claude Code (see section 8.0). Interactive SVG/Canvas charts for Token Velocity, Turn Latency & P95 Distribution, Tool Invocation Matrices, and Session Waterfalls with timeframe selectors (`1H`, `24H`, `7D`, `30D`, `1Y`).
3. **Usage Insights (`#subpanel-insights`)**: Programmatic and workflow-coaching findings across Claude Code, Cursor, and Antigravity (`7D`, `30D`, `90D`), severity/confidence badges, suggested experiments, per-project and per-model token/cache/cost tables, and dataset export instructions.
4. **Tool Bridges (`#subpanel-bridges`, `data-subpanel="bridges"`)**: Placed directly next to `usage insights` inside `#aimon-subnav`, providing global transport settings (`client` / `gateway` / `bidirectional`, host, RPC port `3892`, loopback plugin port `3893`, `~/work` $\leftrightarrow$ `Z:\` path mapping, masked `auth_secret`), four dedicated configuration and live telemetry cards (**Local CPU Accelerators**, **EasyEDA Pro**, **KiCad**, **FreeCAD**), and a rolling 50-call activity log table backed by session-protected `GET /api/eda/status`, `POST /api/eda/config`, and `POST /api/eda/probe`.
5. **Action Approvals (`#subpanel-approvals`)**: Live pending approval actions, 128-bit mobile pairing QR interface, and paired device management.
6. **Dynamic Satellite Panels (`#panel-<id>`)**: Seamless embedded iframe monitors for connected subsystems (`netmon`, `meshmon`, `embdevenv`, `abc80`).

---

## 11. Dual-IDE Hook Protocol

IDE tool approval hooks standardize tool interception across both IDEs:
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
