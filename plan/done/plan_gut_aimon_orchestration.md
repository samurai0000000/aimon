# Plan: Gut Orchestration & Implement Scoped MCP Tool Profiling

- **Date**: 2026-09-20
- **Target Platform**: `builder` (`aimon` daemon, CLI, MCP Server, and Web Dashboard)
- **Status**: Completed (Verified & Deployed)
- **Motivation**:
  1. **Document-Centric Collaboration**: Multi-agent engineering works with 100% reliability through on-disk markdown state machines (`plan_<name>.md` and `Appendix A`) rather than an in-memory server orchestrator. The 19 orchestration tools in `aimon` are unused, introduce deadlocks, and burden every LLM turn.
  2. **Token Burn Reduction**: Currently, `aimon` forces all 67 tools (~7,400 tokens) into every session. In Cursor, where tools are sent on every single API call, a 30-turn session burns over 220,000 input tokens just on tool schemas.
  3. **Strict MCP Primacy & Zero Direct Endpoint Bypasses**: Agents must exclusively use official MCP tools; direct REST API endpoint access remains strictly prohibited and blocked with HTTP 403 Forbidden.
- **Core Objectives**:
  1. **Gut Orchestration**: Purge all 19 orchestration tools, agent chat, token-passing, runners, and checkpoint/interlock code from `aimon`.
  2. **Implement Scoped MCP Tool Profiling**: Add dynamic tool profile filtering (`core`, `embedded`, `network`, `mesh`, `all`) to `aimon`:
     - Default/Core: Only 3 AI quota tools (~150 tokens) for general coding/documentation.
     - Scoped Profiles: Hardware/network tools load only when explicitly requested or working in relevant repos.
  3. **Auto-Scoping via Workspace Roots**: Allow `aimon` to automatically detect client workspace paths (e.g. `amba-virt` -> `embedded`, `netmon` -> `network`, `intelligence` -> `core`).
  4. **Workspace Configuration Templates**: Provide `.cursor/mcp.json` templates to enable Cursor projects to bind to specific profiles.
  5. **Compile & Zero-Regression Validation**: Clean up build definitions, delete dead files, compile cleanly, and verify instantaneous shutdown and clean tool filtering via native stdio MCP mode (no curl/HTTP bypasses).

---

## 1. Inventory of Removals & Retention

### A. Source & Header Files to Delete (12 files)
- `include/AgentMessageBus.hxx` & `src/AgentMessageBus.cxx`
- `include/AgentRunner.hxx` & `src/AgentRunner.cxx`
- `include/CollabOrchestrator.hxx` & `src/CollabOrchestrator.cxx`
- `include/InterlockManager.hxx` & `src/InterlockManager.cxx`
- `include/TranscriptSink.hxx` & `src/TranscriptSink.cxx`
- `include/RunMetrics.hxx` & `src/RunMetrics.cxx`

### B. Obsolete Root Schema Files to Delete (13 files)
- `collab_abort.json`, `collab_get_status.json`, `collab_run.json`, `collab_start.json`, `collab_step.json`
- `exec_end_run.json`, `exec_get_run_metrics.json`, `exec_get_transcript.json`, `exec_interlock_wait.json`, `exec_list_runs.json`, `exec_review_checkpoint.json`, `exec_start_run.json`, `exec_submit_checkpoint.json`
- Obsolete schema caches in `~/.gemini/antigravity-ide/mcp/aimon/` for these 19 tools.

### C. 19 Orchestration Tools to Purge from `McpServer.cxx`
- Task Management: `register_agent_task`, `list_active_tasks`
- Agent Chat & Long Polling: `agent_check_inbox`, `agent_send_reply`, `agent_collaborate`, `agent_wait_turn`
- Execution Checkpoints: `exec_start_run`, `exec_submit_checkpoint`, `exec_review_checkpoint`, `exec_interlock_wait`, `exec_get_transcript`, `exec_end_run`, `exec_list_runs`, `exec_get_run_metrics`
- State Machine Loops: `collab_start`, `collab_run`, `collab_step`, `collab_get_status`, `collab_abort`

### D. What Remains 100% Preserved & Active
- **AI Quota Monitoring**: `check_antigravity_quota`, `check_cursor_usage`, `get_combined_ai_status`.
- **Satellite MCP Tool Gateway (`TcpGateway`)**: All 45 satellite tools (`embdevenv_*`, `lan_*`, `firewall_*`, `snmp_*`, `meshmon_*`).
- **Ncurses Console & Web Dashboard**: Live quota statistics, satellite telemetry, and connected client sessions.
- **REST Endpoint Gating**: All direct REST endpoints (`/api/*`) remain hard-blocked (403 Forbidden) for non-browser clients, enforcing strict MCP tool usage.

---

## 2. Scoped MCP Tool Profiling Architecture

To permanently solve token bloat in Cursor and IDEs, `aimon` will support **Tool Profiles**:

### A. Supported Profiles & Token Costs

| Profile | Included Tools | Target Workspaces | Schema Size / Tokens |
| :--- | :--- | :--- | :--- |
| **`core`** | `check_antigravity_quota`, `check_cursor_usage`, `get_combined_ai_status` | `intelligence`, `aimon`, general coding | ~590 B (**~150 tokens**) |
| **`embedded`** | `core` + `embdevenv_*` (26 tools) | `amba-virt`, `robohero` | ~10.5 KB (~2,600 tokens) |
| **`network`** | `core` + `lan_*`, `firewall_*`, `snmp_*` (13 tools) | `netmon` | ~7.2 KB (~1,800 tokens) |
| **`mesh`** | `core` + `meshmon_*` (6 tools) | `meshmon`, `meshroom`, `meshroof` | ~5.8 KB (~1,450 tokens) |
| **`all`** | All active tools (Default / Multi-domain) | Multi-domain debugging | ~19.6 KB (~4,900 tokens) |

### B. Profile Selection Mechanisms in `aimon`
1. **CLI Flag for Stdio MCP (Native IDE Transport)**:
   - `./build/aimon mcp --profile=core` (used in `.cursor/mcp.json` or Antigravity `mcp_config.json`).
2. **Workspace Root Heuristic (Auto-Scoping)**:
   - When an MCP client sends `initialize` with `rootUri` or `workspaceFolders`:
     - If path contains `amba-virt` or `robohero` -> automatically select `embedded`.
     - If path contains `mesh` -> automatically select `mesh`.
     - If path contains `netmon` -> automatically select `network`.
     - Otherwise -> default to `core` (saving 95% of tokens automatically).
3. **HTTP/SSE Transport Query Parameter & Header (for IDE extensions)**:
   - Query param: `/sse?profile=core` or header `X-Aimon-Profile: core`.
   - Direct REST endpoints (`/api/*`) remain completely disabled for automation.

---

## 3. Detailed Component Modifications

### A. `CMakeLists.txt`
- Remove the 6 deleted `.cxx` files from `set(SOURCES ...)`.

### B. `src/McpServer.cxx` & `include/McpServer.hxx`
- Remove all 19 orchestration tool schemas and dispatch cases.
- Update `handleToolsList(id, profile)` and `handleToolsCall(id, params, profile)`:
  - Always output the 3 core quota tools.
  - Filter dynamic satellite tools based on active profile:
    - `core`: skip all dynamic tools.
    - `embedded`: include only tools matching prefix `embdevenv_`.
    - `network`: include only tools matching prefix `lan_`, `firewall_`, `snmp_`.
    - `mesh`: include only tools matching prefix `meshmon_`.
    - `all`: include all tools.

### C. `src/WebServer.cxx` & `include/WebServer.hxx`
- Extract `profile` from query params (`/sse?profile=...`, `/message?profile=...`) and headers.
- Pass active profile into `McpServer`.
- Remove dead route handlers under `/api/collaboration/*`, `/api/exec/*`, and `/api/tasks/*`.
- Simplify `WebServer::stop()`.

### D. `src/NcursesConsole.cxx` & `include/NcursesConsole.hxx`
- Remove `collab` CLI subcommands and `chat` command.
- Remove collaboration status banner from the top header window (`| Collab: [ ... ]`).
- Remove `_collabOrch` reference.

### E. `src/Main.cxx`
- Remove CLI `collab <subcmd>` invocation logic and usage text.
- Add `--profile` option to CLI `aimon mcp`.
- Remove `collabOrchestrator` instantiation and shutdown hooks.

### F. `src/TaskRegistry.cxx` & `include/TaskRegistry.hxx`
- Remove `AgentTask` storage methods.
- Retain `ClientSession` tracking (`registerSession`, `removeSession`, `listSessions`) for connected MCP client metrics.

### G. `include/Models.hxx`
- Purge dead struct definitions (`AgentTask`, `CollaborationSession`, `InterlockRequest`, `RunRecord`, etc.).

### H. `web/index.html` & `web/app.js`
- Remove dead `<section class="exec-runs-card">` from `web/index.html`.
- Remove dead polling fetches to `/api/exec/*` and `/api/tasks` from `web/app.js` to eliminate 404s.

### I. Documentation & Ecosystem Alignment (`aimon` & `intelligence`)
- **`aimon/README.md`**:
  - Document the new `?profile=core|embedded|network|mesh|all` query parameter in Cursor and Antigravity SSE configurations.
  - Document CLI stdio `--profile=<name>` flag.
  - Highlight token savings (up to 98% reduction for pure software/quota monitoring).
  - Update project directory tree to remove deleted orchestration files.
- **`aimon/doc/Design.md`**:
  - Remove dead references to `register_agent_task` and orchestration tools.
  - Document the scoped tool profiling architecture and gateway design.
- **`aimon/TODO.md`**:
  - Explicitly document that the "Cursor–Gemini Hardware Orchestration via aimon" server prototype has been retired and superseded by the repo-level document state machine (`start-collaboration-plan` + `plan_<name>.md` + `Appendix A`).
- **`intelligence/selfso/Projects.md`**:
  - Update `aimon` mission description to reflect its streamlined role as the central AI quota monitor and satellite MCP gateway.
  - Document the scoped tool profiles.
- **`intelligence/selfso/InstructionsForAgents.md`**:
  - Update `aimon` MCP Gateway section to document available tool profiles and token-saving guidance.
- **`intelligence/selfso/agents/rules/mcp-tool-usage.md` & `intelligence/selfso/cursor/rules/mcp-tool-usage.mdc`**:
  - Maintain 1-to-1 parity: Add guidance on configuring scoped tool profiles for workspaces to optimize token burn.
  - Reiterate that peer collaboration uses `start-collaboration-plan` rather than server orchestration tools.

---

## 4. Staged Implementation Steps

### Phase 1: Models & TaskRegistry Simplification
- Clean up `include/Models.hxx` to remove dead structs.
- Simplify `TaskRegistry` to focus strictly on `ClientSession` tracking.

### Phase 2: Purge Orchestration from McpServer & WebServer
- Strip the 19 orchestration tools from `src/McpServer.cxx`.
- Remove dead API routes from `src/WebServer.cxx`.

### Phase 3: Implement Tool Profiling Engine in McpServer & WebServer
- Implement profile filtering in `McpServer.cxx` (`core`, `embedded`, `network`, `mesh`, `all`).
- Connect profile extraction in `WebServer.cxx` and stdio `McpServer`.

### Phase 4: CLI, Ncurses & Web Dashboard Cleanup
- Remove `collab` commands and collab header from `src/NcursesConsole.cxx` and `include/NcursesConsole.hxx`.
- Remove collab CLI and references from `src/Main.cxx`. Add `--profile` support to stdio `mcp`.
- Clean dead execution cards in `web/index.html` and fetch calls in `web/app.js`.

### Phase 5: Delete Obsolete Files & Update Build
- Delete the 12 C++ source/header files.
- Delete the 13 root schema JSON files and clean MCP cache.
- Update `CMakeLists.txt`.

### Phase 6: Documentation & Ecosystem Updates
- Update `aimon/README.md`, `aimon/doc/Design.md`, and `aimon/TODO.md`.
- Update `intelligence/selfso/Projects.md` and `intelligence/selfso/InstructionsForAgents.md`.
- Update `intelligence/selfso/agents/rules/mcp-tool-usage.md` and `intelligence/selfso/cursor/rules/mcp-tool-usage.mdc` (maintaining 1-to-1 parity).

### Phase 7: Verification & Deployment
- Compile natively on `builder`: `make -j$(nproc)`.
- Verify `git diff --check` is clean across both `aimon` and `intelligence`.
- Verify tool profiling via native Stdio MCP mode (no HTTP bypasses):
  ```bash
  echo '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' | ./build/aimon mcp --profile=core
  ```
  Confirm returns exactly 3 tools (`check_antigravity_quota`, `check_cursor_usage`, `get_combined_ai_status`).
  ```bash
  echo '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' | ./build/aimon mcp --profile=embedded
  ```
  Confirm returns 3 quota + 26 `embdevenv_*` tools.
- Restart `aimon` daemon in screen session `aimon` cleanly:
  ```bash
  screen -S aimon -X stuff $'\003'
  screen -S aimon -X stuff "./build/aimon daemon --port 3883\n"
  ```
- Verify instant, graceful shutdown on `SIGINT` (zero hangs, zero kill -9).

