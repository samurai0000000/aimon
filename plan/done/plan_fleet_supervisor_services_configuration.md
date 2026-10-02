# Plan: Fleet Supervisor & Satellite Services Configuration

- **Date**: 2026-09-27
- **Target Platform / Scope**: `aimon` (`~/.config/aimon/config.json`, `ServiceSupervisor`, Fleet Dashboard & MCP Gateway)
- **Status**: Done
- **Lifecycle Location**: `plan/done/`
- **Artifacts Directory**: `plan/plan_fleet_supervisor_services_configuration.artifacts/`
- **Agent Mode**: Single-Agent Pair Programming
- **Core Objectives**:
  1. Populate `~/.config/aimon/config.json` with the canonical `supervisor` and `services` configuration for `meshmon` (`fox:16880`), `netmon` (`rhino:3884`), and `embdevenv` (`rhino:3886`).
  2. Reload the `aimon` daemon running in GNU screen session `aimon` on `builder` to initialize `ServiceSupervisor`.
  3. Validate live active health probes, HTTP REST endpoints (`/api/services`, `/api/supervisor/status`), MCP tools (`service_list`, `service_status`), and Dashboard fleet card state (`3/3 Healthy`).

---

## 0. Corrections & Negative Constraints (Do Not Reintroduce)

1. **Do Not Kill GNU Screen Sessions**: Never destroy the GNU screen sessions (`aimon`, `meshmon`, `netmon`, `embdevenv`). Follow the process interruption and relaunch protocol in `InstructionsForAgents.md`.
2. **Do Not Modify Satellite Binaries Unnecessarily**: The satellite binaries (`meshmon`, `netmon`, `embdevenv`) are healthy, running, and listening on their ports. Do not restart or recompile them; only update `aimon`'s configuration and reload `aimon`.
3. **Preserve Existing Configuration Fields**: Preserve all existing fields in `~/.config/aimon/config.json` (`antigravity`, `cursor`, `history`, `mqtt`, `polling`, `web`, `collaboration`).

---

## 1. Execution Boundaries & Strict Guardrails

During execution, the assistant operates strictly under these boundaries:

1. **Direct Question Answering (Virtual Ask Mode)**: Answer inquiries in conversational markdown using read-only tools.
2. **Mandatory User Authorization Gate (Virtual Plan Mode)**: Never modify configuration files or execute modifying daemon commands without an approved plan and explicit user instruction to proceed.
3. **Plan Artifacts Directory**: Save supplementary logs and verification artifacts under `plan/plan_fleet_supervisor_services_configuration.artifacts/`.
4. **Zero-Tolerance Guessing & Speculative Workarounds**: Verify all commands, hostnames, ports, and JSON keys against the codebase and `InstructionsForAgents.md`.
5. **Ground Truth & Real Verification**: Validate with live socket checks, HTTP responses from `/api/services`, and live MCP tool invocations (`service_list`, `service_status`).
6. **Git Commit Prohibition**: Never run `git commit` without explicit instruction from the user.

---

## 2. Technical Approach & Architecture

### 2.1 Purpose and Acceptance Criteria
- Configure `aimon`'s `ServiceSupervisor` to monitor the 3 satellite services across the fleet.
- Ensure `/api/services` returns the 3 services with status `HEALTHY` and non-zero probe latencies.
- Ensure MCP tools `service_list` and `service_status` succeed with accurate real-time metrics.
- Ensure the `aimon` Web Dashboard fleet card displays `3/3 Healthy` with green status dot.

### 2.2 System Architecture Diagram
```
┌────────────────────────────────────────────────────────────────────────┐
│                        builder (127.0.0.1)                             │
│  aimon daemon (screen: 'aimon', Web Port 3883, TCP Gateway 3885)       │
│  ├── ServiceSupervisor (polls TCP ports every 5s)                      │
│  │   ├── probe fox:16880 (meshmon) ────────> fox (192.168.8.245)      │
│  │   ├── probe rhino:3884 (netmon)  ───────> rhino (192.168.8.30)     │
│  │   └── probe rhino:3886 (embdevenv) ─────> rhino (192.168.8.30)     │
│  ├── /api/services (returns 3/3 HEALTHY)                               │
│  └── MCP Server (service_list, service_status)                         │
└────────────────────────────────────────────────────────────────────────┘
```

### 2.3 Configuration Schema (`~/.config/aimon/config.json`)
```json
{
  "supervisor": {
    "enabled": true,
    "auto_restart": true,
    "probe_interval_sec": 5,
    "probe_timeout_ms": 800,
    "crash_loop_window_sec": 60,
    "crash_loop_max_retries": 3,
    "backoff_initial_sec": 5,
    "backoff_max_sec": 60,
    "safe_mode_port": 3889
  },
  "services": [
    {
      "id": "meshmon",
      "name": "Mesh Monitor",
      "host": "192.168.8.245",
      "port": 16880,
      "secondary_port": 0,
      "enabled": true,
      "probe_interval_ms": 5000,
      "probe_timeout_ms": 800,
      "max_restart_retries": 3,
      "restart_window_sec": 60,
      "start_cmd": "ssh -n fox 'cd ~/work/meshmon && ./build/aarch64/meshmon -b -D ~/.config/meshmon/meshmon.db >> ~/.config/meshmon/meshmon.log 2>&1'",
      "stop_cmd": "ssh -n fox 'pkill -SIGTERM -f build/aarch64/meshmon'",
      "status_cmd": "ssh -n fox 'pgrep -f build/aarch64/meshmon'",
      "pid_file": "~/.config/meshmon/meshmon.pid",
      "log_file": "~/.config/meshmon/meshmon.log"
    },
    {
      "id": "netmon",
      "name": "Network Monitor",
      "host": "192.168.8.30",
      "port": 3884,
      "secondary_port": 0,
      "enabled": true,
      "probe_interval_ms": 5000,
      "probe_timeout_ms": 800,
      "max_restart_retries": 3,
      "restart_window_sec": 60,
      "start_cmd": "ssh -n rhino 'cd ~/work/netmon && ./build/netmon daemon -b >> ~/.config/netmon/netmon.log 2>&1'",
      "stop_cmd": "ssh -n rhino 'pkill -SIGTERM -f build/netmon'",
      "status_cmd": "ssh -n rhino 'pgrep -f build/netmon'",
      "pid_file": "~/.config/netmon/netmon.pid",
      "log_file": "~/.config/netmon/netmon.log"
    },
    {
      "id": "embdevenv",
      "name": "Embedded Dev",
      "host": "192.168.8.30",
      "port": 3886,
      "secondary_port": 0,
      "enabled": true,
      "probe_interval_ms": 5000,
      "probe_timeout_ms": 800,
      "max_restart_retries": 3,
      "restart_window_sec": 60,
      "start_cmd": "ssh -n rhino 'cd ~/work/embdevenv && ./build/embdevenv -b >> ~/.config/embdevenv/embdevenv.log 2>&1'",
      "stop_cmd": "ssh -n rhino 'pkill -SIGTERM -f build/embdevenv'",
      "status_cmd": "ssh -n rhino 'pgrep -f build/embdevenv'",
      "pid_file": "~/.config/embdevenv/embdevenv.pid",
      "log_file": "~/.config/embdevenv/embdevenv.log"
    }
  ]
}
```

---

## 3. Staged Implementation Plan & Acceptance Criteria

### 3.0 Mandatory Qualification Protocol & Strict Progression Gate Rule

All envelopes must be verified against physical ground truth. If verification fails, stop immediately, report facts, and resolve before continuing.

---

### Envelope 1: Update Configuration & Relaunch `aimon`

**Goal:** Merge `supervisor` and `services` configuration into `~/.config/aimon/config.json` and restart `aimon` inside GNU screen session `aimon` on `builder`.

Planned files:
- `~/.config/aimon/config.json` — add `supervisor` and `services` blocks.

- [x] Task 1.1: Read and safely update `~/.config/aimon/config.json` while preserving existing `cursor`, `antigravity`, `mqtt`, and `collaboration` settings.
  - **Target Files**: `~/.config/aimon/config.json`
  - **Verification**: `python3 -m json.tool ~/.config/aimon/config.json` validates clean JSON syntax.
- [x] Task 1.2: Cleanly restart `aimon` inside screen session `aimon` on `builder`:
  ```bash
  screen -S aimon -X stuff $'\003'
  sleep 1
  screen -S aimon -X stuff "./build/aimon daemon --port 3883\n"
  ```
  - **Verification**: `ss -tlpn | grep 3883` confirms active listener on port 3883.

**Hardstop:** Do not advance to Envelope 2 until `aimon` is running and listening on port 3883.

---

### Envelope 2: Live Ground-Truth Qualification & MCP Validation

**Goal:** Verify that `ServiceSupervisor` probes all 3 services, populates `/api/services`, and serves MCP tools accurately.

- [x] Task 2.1: Query `GET http://127.0.0.1:3883/api/services` and `GET http://127.0.0.1:3883/api/supervisor/status`.
  - **Verification**: Output shows 3 services (`meshmon`, `netmon`, `embdevenv`) in state `HEALTHY`.
- [x] Task 2.2: Invoke MCP tools `service_list` and `service_status` via `aimon`.
  - **Verification**: `service_list` returns table with all 3 services marked `HEALTHY`; `service_status` for each service returns JSON runtime status.

**Hardstop:** Complete when all 3 services are verified `HEALTHY` in both REST and MCP interfaces.

---

## 3.8 Mandatory Verification and Validation Gates Reference

#### 3.8.1 Fleet Service Supervision Gate (`/api/services` & MCP)
- **Nominal Coverage**: `GET /api/services` returns array of 3 objects with `state: "HEALTHY"`, latency < 10ms.
- **MCP Tool Coverage**: `service_list` returns 3 monitored daemons; `service_status` returns structured status for `meshmon`, `netmon`, `embdevenv`.
- **Acceptance Threshold**: 3/3 services healthy, 0 errors.

---

## 3.9 Completion Definition

The implementation is complete when:
1. `~/.config/aimon/config.json` contains valid `supervisor` and `services` definitions;
2. `aimon` daemon on `builder` is running with `ServiceSupervisor` active;
3. `service_list` and `/api/services` report `3/3 Healthy` for `meshmon`, `netmon`, and `embdevenv`;
4. Verification evidence is reported to the user.

---

## Appendix. Lifecycle Transition & Status Log

| Date | Previous State | New State | Lifecycle Directory | Notes / Rationale |
|---|---|---|---|---|
| 2026-09-27 | — | `Proposed` | `plan/` | Initial plan drafted to configure fleet supervisor and services |
| 2026-09-27 | `Proposed` | `Executing` | `plan/` | Approved by user; config merged, aimon restarted, 3/3 services verified HEALTHY |
| 2026-09-27 | `Executing` | `Done` | `plan/done/` | Outcome accepted by user; plan finalized and archived to plan/done/ |

### Checkpoints & Iteration Log
- 2026-09-27 11:53: `~/.config/aimon/config.json` updated with supervisor and services blocks; JSON syntax verified.
- 2026-09-27 11:53: Orphaned background daemon terminated; `aimon` cleanly restarted in screen session `aimon` on `builder`.
- 2026-09-27 11:54: REST endpoints `/api/services`, `/api/supervisor/status`, and `/api/monitors` verified. 3/3 services (`meshmon`, `netmon`, `embdevenv`) reporting state `HEALTHY` with 1 ms probe latency.
- 2026-09-27 11:54: MCP JSON-RPC tools (`service_list`, `service_status`, `lan_get_devices`, `lan_get_traffic_summary`, `firewall_get_status`) verified.

