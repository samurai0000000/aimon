# Implementation Plan: Web UI Session-Gated REST Endpoints with Exact MCP Tool Guidance & Production Hardening

## 1. Operational Constraints & Non-Disruption Directive

> [!CAUTION]
> **STRICT ZERO-DISRUPTION MANDATE ACROSS ALL TARGET HOSTS**:
> - **`builder` (`127.0.1.1` / local)**: Running `aimon` inside GNU screen session `aimon`.
> - **`fox` (`192.168.8.245`, Raspberry Pi)**: Running `meshmon` inside GNU screen session `meshmon` actively sniffing Meshtastic LoRa radios.
> - **`rhino` (`192.168.8.30`)**: Running `netmon` inside GNU screen session `netmon` and `embdevenv` managing active hardware testbeds.
> - **UNDER NO CIRCUMSTANCES** should any process or GNU screen session on `builder`, `fox`, or `rhino` be killed, interrupted (`^C`), signaled, or restarted without explicit approval.
> - Compilation will occur cleanly without interfering with currently mapped executable memory or running services.

---

## 2. Lessons Learned from Recent Incidents & Edge-Case Bugs

During the implementation, deployment, and live hardware verification of `embdevenv` on `rhino`, five critical edge-case failure modes were discovered. These architectural lessons are now mandatory requirements for `aimon`, `meshmon`, and `netmon`:

### A. Mutex Re-entrancy & Self-Deadlock in Streaming / Logging Subsystems
- **Incident**: Calling `startLogFile()` while holding `std::mutex _fileLogMutex` invoked `stopLogFile()`, which attempted to acquire the same non-recursive mutex, resulting in immediate self-deadlock. In the JSON-RPC dispatch thread, this halted the entire client loop and caused downstream `aimon` gateway calls to time out (`TcpGateway.cxx:421: Timed out waiting for subsystem`).
- **Architectural Requirement**:
  - All streaming sinks, file loggers, and background thread controllers must use `std::recursive_mutex` or strictly separate public locking wrappers from internal `*Locked()` helpers.
  - Mutexes must never be held when invoking external callbacks, listener dispatches, or nested lifecycle methods.

### B. Defensive Parameter Aliasing in Tool Dispatchers
- **Incident**: LLM agents frequently substitute synonym argument keys across tools (e.g. passing `{"target": "...", "input": "..."}` instead of `"data"`). A rigid dispatcher extracting only `args.value("data", "")` evaluated to an empty string, causing zero-length writes to fail with cryptic `-1` status codes.
- **Architectural Requirement**:
  - All tool dispatchers across `aimon`, `meshmon`, and `netmon` must accept common parameter aliases (e.g., `data`, `input`, `command`, `text`, `payload` for writes; `target`, `node`, `device` for addressing; `window_days`, `days`, `hours` for query spans).
  - Empty or missing required payloads must return explicit, actionable JSON errors (e.g. `"No data provided. Please specify 'data' or 'input' parameter."`) rather than failing silently or triggering false I/O errors.
  - Zero-length writes at the transport layer must return `0` (success with zero bytes transferred), never `-1`.
  - Tool input schemas must explicitly document accepted aliases.

### C. Terminal Emulator CPR / DA Automated Response Filtering
- **Incident**: When firmware/UEFI queried cursor position via `\x1b[6n`, an attached interactive Telnet client running in a terminal emulator automatically replied with Cursor Position Reports (`\x1b[28R`, `\x1b[20R`). The daemon forwarded these bytes directly to the target UART, corrupting bootloader prompts and causing `Unknown command '\x1b[28R'`.
- **Architectural Requirement**:
  - Any server exposing interactive terminal access (Telnet, TCP socket shells) must filter automated terminal emulator responses:
    - Cursor Position Reports: `\x1b[<row>;<col>R` and `\x1b[<row>R`
    - Primary/Secondary Device Attributes: `\x1b[?<attrs>c` and `\x1b[<attrs>c`
  - Interactive operator keystrokes (ANSI arrow keys `\x1b[A..D`, function keys `\x1b[...~`, Home/End) must remain unmodified and fully responsive.

### D. Web UI Session Integrity: Cookie + Header + User-Agent Filtering
- **Incident**: A naive blanket 403 on `/api/*` broke the web dashboard because browser `fetch()` polling was rejected alongside agent scripts.
- **Architectural Requirement**:
  - `GET /` serves HTML and issues a dynamic session token via `Set-Cookie: <daemon>_session=<token>` while injecting `window.__UI_SESSION_TOKEN__ = "<token>";`.
  - Frontend JavaScript intercepts `window.fetch` to attach `X-UI-Session: <token>`.
  - Pre-routing inspection checks session validity (`Cookie` OR `X-UI-Session` header) and blocks known automation User-Agents (`curl/`, `python/`, `wget/`, `Go-http-client/`).
  - Blocked requests receive `HTTP 403 Forbidden` with URL-specific, copy-paste-ready MCP tool hints.

### E. Telemetry Panel Layout & Total Scrollbar Elimination
- **Incident**: Shrinking voltage/metric containers or setting fixed max-heights eliminated horizontal scrollbars but introduced unsightly vertical scrollbars within the telemetry panel.
- **Architectural Requirement**:
  - Sidebar width widened appropriately (e.g. `360px` or flex equivalent).
  - Metric grids use a 3-column layout (`repeat(3, 1fr)`) with `max-height: none; overflow: visible;`.
  - Inner badges enforce `white-space: nowrap; overflow: hidden; text-overflow: ellipsis;`.
  - Result: completely eliminates both horizontal and vertical scrollbars.

### F. WebServer Shutdown Hangs Caused by Infinite Socket / WebSocket Read Timeouts
- **Incident**: Open browser WebSocket connections or TCP client sockets with infinite read timeouts blocked `server->stop()` indefinitely during daemon shutdown, preventing clean restart.
- **Architectural Requirement**:
  - Configure bounded read timeouts (e.g. 1 second) on WebSocket and client socket read loops.
  - Check the daemon's `_running` liveness flag within every connection loop to ensure immediate, clean worker thread termination upon shutdown.

### G. Cross-Host Single-Instance Lock Collisions across NFS-Mounted Configurations
- **Incident**: In a multi-host ecosystem (`builder`, `fox`, `rhino`) sharing `/home` via NFS, a static lockfile name (e.g. `daemon.pid`) caused false collisions, where a daemon running on `rhino` blocked launch on `builder`.
- **Architectural Requirement**:
  - Enforce host-specific locking: `<daemon>.<hostname>.pid` in the runtime directory.
  - Use POSIX advisory file locking (`fcntl` / `flock`) and verify process liveness via `/proc/<pid>` or `kill(pid, 0)` to safely reclaim stale locks after unclean terminations.

### H. Asynchronous Subprocess Execution & Pipe EOF Deadlock Prevention
- **Incident**: Invoking external commands or flash scripts via blocking `fgets()`/`pclose()` stalled when grandchild processes kept standard streams open or processes hung, deadlocking the JSON-RPC/MCP dispatch thread and causing client timeouts.
- **Architectural Requirement**:
  - Replace blocking pipes with non-blocking `select()` / `poll()` loops bounded by monotonic deadlines and `SIGKILL` timeouts (`kill -9` or `-k 5s`).
  - Long operations (>2s) must execute asynchronously, updating an in-memory status record (`_lastResult`), while exposing an instant polling tool (e.g. `embdevenv_flash_get_status`, `exec_get_run_metrics`).

### I. Dynamic Hardware Topology Binding vs. Devnode Shifts
- **Incident**: Serial and USB devnodes (`/dev/ttyUSB0`, `/dev/ttyACM0`) shift dynamically when USB hubs reset or devices power cycle, pointing tools to the wrong physical ports.
- **Architectural Requirement**:
  - Bind serial and USB devices dynamically using physical sysfs device topology (e.g., USB hub port hierarchy `/sys/bus/usb/devices/...`) or deterministic USB vendor/product/serial IDs, never assuming static `/dev/tty*` indices.

### J. Per-Entity Lifetime Statistics & Event History Storage with Unbounded Window Support
- **Incident**: Aggregated stats dashboards lacked fine-grained per-target analytics, while bottom-tab clumping cluttered UI navigation. Furthermore, querying all-time history was unsupported when queries strictly required positive day windows.
- **Architectural Requirement**:
  - Store lifecycle events (flash, reboot, power cycles, recovery triggers) in SQLite with WAL mode.
  - Support `window_days <= 0` to denote all-time/unbounded history queries without time cutoffs.
  - Expose dedicated `/stats` and `/events` endpoints with official MCP tool equivalents (`embdevenv_get_board_stats`, `meshmon_get_rf_analytics`, `snmp_get_device_metrics`).
  - Render stats cards directly on each entity's tab/panel with lifetime counters and category filter chips, avoiding bottom-tab sprawl.

### K. Standing Condition Variable Busy-Wait CPU Spikes
- **Incident**: In `aimon` `CollabOrchestrator`, a condition variable predicate checked a standing flag (`_autoDrive == true`) rather than a state-change/work-needed signal, causing the wait loop to evaluate immediately and spin at 100% CPU.
- **Architectural Requirement**:
  - `std::condition_variable::wait_for()` predicates must only evaluate true when work is queued or when terminating (e.g. `!_running || _stepRequested || !_queue.empty()`). Standing configuration or mode flags must never bypass the sleep wait.

### L. Strict MCP Tool Failure Protocol (Stop-and-Report)
- **Incident**: AI agents encountering broken or failing MCP tools historically attempted speculative shell scripts, direct telnet hacks, or unauthorized REST curls that corrupted hardware states or bypassed security controls.
- **Architectural Requirement**:
  - Mandated across ecosystem rules (`intelligence/selfso` and `intelligence/ambarella`): On any MCP tool failure or unexpected output, agents must pause immediately and report the failure with structured context. Workarounds and alternative backdoors are strictly prohibited.

---

## 3. URL-to-MCP Tool Mapping Matrix

### A. `aimon` (`port 3883`)

| Request URL / Pattern | Specific MCP Tool Hint |
| :--- | :--- |
| `GET /api/status`, `GET /api/history` | `"Use official MCP tool 'get_combined_ai_status', 'check_antigravity_quota', or 'check_cursor_usage'."` |
| `GET /api/tasks`, `POST /api/tasks/*` | `"Use official MCP tool 'list_active_tasks' or 'register_agent_task'."` |
| `POST /api/collaboration/start` | `"Use official MCP tool 'collab_start' with argument {\"plan_file\": \"...\"}."` |
| `POST /api/collaboration/run` | `"Use official MCP tool 'collab_run' with argument {\"plan_file\": \"...\"}."` |
| `POST /api/collaboration/step` | `"Use official MCP tool 'collab_step'."` |
| `GET /api/collaboration/status` | `"Use official MCP tool 'collab_get_status'."` |
| `POST /api/collaboration/abort` | `"Use official MCP tool 'collab_abort'."` |
| `GET /api/exec/runs` | `"Use official MCP tool 'exec_list_runs'."` |
| `POST /api/exec/runs` | `"Use official MCP tool 'exec_start_run'."` |
| `GET /api/exec/runs/:id/transcript` | `"Use official MCP tool 'exec_get_transcript' with argument {\"run_id\": \"<id>\"}."` |
| `GET /api/exec/runs/:id/metrics`, `GET /api/exec/runs/:id/stats` | `"Use official MCP tool 'exec_get_run_metrics' with argument {\"run_id\": \"<id>\"}."` |
| `GET /api/exec/interlocks` | `"Use official MCP tool 'exec_interlock_wait'."` |
| `POST /api/exec/interlocks/:id/resolve` | `"Use official MCP tool 'exec_review_checkpoint' with argument {\"run_id\": \"...\", \"decision\": \"...\"}."` |

### B. `meshmon` (`port 16880`)

| Request URL / Pattern | Specific MCP Tool Hint |
| :--- | :--- |
| `GET /api/status`, `GET /api/nodes`, `GET /api/node` | `"Use official MCP tool 'meshmon_get_node_status'."` |
| `GET /api/nodes/:id/stats`, `GET /api/nodes/:id/events` | `"Use official MCP tool 'meshmon_get_rf_analytics' or 'meshmon_get_node_status' with argument {\"node_id\": \"<id>\"}."` |
| `GET /api/analytics` | `"Use official MCP tool 'meshmon_get_rf_analytics' with argument {\"hours\": 24}."` |
| `GET /api/packets` | `"Use official MCP tool 'meshmon_query_telemetry_history' or 'meshmon_get_rf_analytics'."` |
| `GET /api/spatial` | `"Use official MCP tool 'meshmon_get_spatial_analytics'."` |
| `POST /api/messages/send`, `GET /api/messages` | `"Use official MCP tool 'meshmon_send_message' with argument {\"text\": \"...\"}."` |
| `POST /api/db/query` | `"Use official MCP tool 'meshmon_query_db' with argument {\"query\": \"...\"}."` |

### C. `netmon` (`port 3884`)

| Request URL / Pattern | Specific MCP Tool Hint |
| :--- | :--- |
| `GET /api/status` | `"Use official MCP tool 'snmp_get_wan_status' or 'lan_get_traffic_summary'."` |
| `GET /api/snmp/wan` | `"Use official MCP tool 'snmp_get_wan_status'."` |
| `GET /api/snmp/devices` | `"Use official MCP tool 'snmp_get_device_metrics' with argument {\"filter\": \"monitored\"}."` |
| `GET /api/devices/:mac/stats`, `GET /api/devices/:mac/events` | `"Use official MCP tool 'snmp_get_device_metrics' with argument {\"mac\": \"<mac>\"}."` |
| `GET /api/snmp/history`, `GET /api/snmp/*` | `"Use official MCP tool 'snmp_query_oid' or 'snmp_get_interface_counters'."` |
| `GET /api/traffic`, `GET /api/traffic/top-talkers` | `"Use official MCP tool 'lan_get_top_talkers' or 'lan_get_traffic_summary'."` |
| `GET /api/devices` | `"Use official MCP tool 'lan_get_devices' or 'lan_get_unregistered_devices'."` |
| `POST /api/devices/name` | `"Use official MCP tool 'lan_name_device' with argument {\"mac\": \"...\", \"name\": \"...\"}."` |
| `GET /api/firewall/status` | `"Use official MCP tool 'firewall_get_status'."` |
| `GET /api/firewall/sessions` | `"Use official MCP tool 'firewall_get_sessions'."` |
| `POST /api/firewall/block` | `"Use official MCP tool 'firewall_block_ip' with argument {\"ip\": \"...\"}."` |
| `POST /api/firewall/unblock` | `"Use official MCP tool 'firewall_unblock_ip' with argument {\"ip\": \"...\"}."` |

### D. `embdevenv` (`port 3886` on `rhino`)

| Request URL / Pattern | Specific MCP Tool Hint |
| :--- | :--- |
| `GET /api/status`, `GET /api/targets` | `"Use official MCP tool 'embdevenv_get_target_status' or 'embdevenv_list_targets'."` |
| `GET /api/targets/:name/stats` | `"Use official MCP tool 'embdevenv_get_board_stats' with argument {\"target\": \"<name>\"}."` |
| `GET /api/targets/:name/events` | `"Use official MCP tool 'embdevenv_get_board_stats' or 'embdevenv_console_read' with argument {\"target\": \"<name>\"}."` |
| `GET /api/usb/devices` | `"Use official MCP tool 'embdevenv_usb_list_devices'."` |
| `POST /api/targets/:name/mcu/power` | `"Use official MCP tool 'embdevenv_mcu_power_on', 'embdevenv_mcu_power_off', or 'embdevenv_mcu_reboot' with argument {\"target\": \"<name>\"}."` |
| `POST /api/targets/:name/mcu/command` | `"Use official MCP tool 'embdevenv_mcu_send_command' with argument {\"target\": \"<name>\", \"command\": \"...\"}."` |
| `GET /api/targets/:name/mcu/telemetry*` | `"Use official MCP tool 'embdevenv_mcu_get_telemetry' or 'embdevenv_mcu_get_telemetry_history' with argument {\"target\": \"<name>\"}."` |
| `POST /api/targets/:name/console/write` | `"Use official MCP tool 'embdevenv_console_write' or 'embdevenv_console_expect' with argument {\"target\": \"<name>\", \"data\": \"...\"}."` |
| `POST /api/targets/:name/console/clear` | `"Use official MCP tool 'embdevenv_console_clear' with argument {\"target\": \"<name>\"}."` |
| `GET /api/targets/:name/console` | `"Use official MCP tool 'embdevenv_console_read' or 'embdevenv_console_catch_bootloader' with argument {\"target\": \"<name>\"}."` |
| `POST /api/targets/:name/flash` | `"Use official MCP tool 'embdevenv_flash_target' with argument {\"target\": \"<name>\"}."` |
| `GET /api/targets/:name/flash/status` | `"Use official MCP tool 'embdevenv_flash_get_status' with argument {\"target\": \"<name>\"}."` |

---

## 4. Implementation Specifications per Daemon

### 1. WebServer Pre-Routing Gate & Exact MCP Tool Hints
In `WebServer.cxx` for each project:
- Add configuration `web.endpoints_enabled` (default `false`).
- In `setupRoutes()`, register `set_pre_routing_handler`:
  ```cpp
  if (!_endpointsEnabled) {
      _server->set_pre_routing_handler([this](const httplib::Request &req, httplib::Response &res) {
          if (req.path == "/api" || req.path.rfind("/api/", 0) == 0) {
              if (!isValidUiSession(req)) {
                  res.status = 403;
                  std::string hint = getMcpHintForPath(req.path, req.body);
                  nlohmann::json err = {
                      {"error", "Direct REST API endpoint access is disabled by configuration."},
                      {"hint", hint},
                      {"daemon", "<daemon_name>"}
                  };
                  res.set_content(err.dump(2), "application/json");
                  return httplib::Server::HandlerResponse::Handled;
              }
          }
          return httplib::Server::HandlerResponse::Unhandled;
      });
  }
  ```
- Implement `getMcpHintForPath(path, body)` matching the table in Section 3.
- Issue dynamic session tokens on `GET /` with `Set-Cookie` and `window.__UI_SESSION_TOKEN__`.
- Inject frontend fetch interceptor in web dashboard assets.

### 2. Defensive Tool Dispatcher Argument Parsing
In `ToolDispatcher.cxx` / `McpServer.cxx`:
- When parsing parameters, inspect aliases sequentially:
  ```cpp
  std::string data = args.value("data", "");
  if (data.empty() && args.contains("input") && args["input"].is_string()) {
      data = args["input"].get<std::string>();
  }
  if (data.empty() && args.contains("command") && args["command"].is_string()) {
      data = args["command"].get<std::string>();
  }
  if (data.empty() && args.contains("text") && args["text"].is_string()) {
      data = args["text"].get<std::string>();
  }
  ```
- Guard against zero-length writes and return clear error descriptions.
- Update tool schema JSON files in `~/.gemini/antigravity-ide/mcp/` to document supported aliases.

### 3. Mutex Re-entrancy & Thread Safety
- Audit all mutex usages in streaming classes (e.g. `TranscriptSink`, `PacketLogger`, `SnmpDatabase`).
- Ensure no lock is held when calling public teardown methods or notifying callbacks.
- Use `std::recursive_mutex` for classes with nested lifecycle methods.

### 4. Automated Terminal Response Filtering in Sockets/Telnet
- For any service handling interactive console streams, filter ANSI CPR (`\x1b[...R`) and DA (`\x1b[?...c`) sequences from inbound client streams.

### 5. UI Scrollbar Elimination
- Apply 3-column grid structure with `max-height: none; overflow: visible;`.
- Set container `overflow-x: hidden;` and text badge `white-space: nowrap; text-overflow: ellipsis;`.

---

## 5. Non-Disruptive Verification Plan

1. **Compilation**:
   - Build `aimon` natively on `builder`: `make -j$(nproc)`.
   - Build `netmon` on `builder`: `make -j$(nproc)`.
   - Build `meshmon` natively on `fox`: `ssh -n fox "cd ~/work/meshmon && make -j$(nproc)"`.
2. **Curl Verification**:
   - Issue direct curl commands to each daemon's port (`3883`, `16880`, `3884`).
   - Confirm `HTTP 403 Forbidden` is returned with exact MCP tool name and arguments in `hint`.
3. **Web Browser Verification**:
   - Access `GET /` in browser.
   - Confirm 200 OK, active session token, live telemetry streaming, and **zero vertical or horizontal scrollbars** in telemetry panels.
4. **Tool Alias Verification**:
   - Invoke MCP tools passing alternate parameter names (`input` vs `data`, `target` vs `node`) and verify success.
