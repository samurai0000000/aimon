# Execution Run Metrics, Waterfall Telemetry & Zero-Curl MCP Orchestration

## 1. Problem Statement

During hardware bring-up sessions (e.g. N1-655 camera bringing up DSP, IAV, SerDes, and 3A), multi-agent execution involved dozens of commands across 8+ iterations. With Iterations 1 & 2 (`TranscriptSink` and `InterlockManager`) operational, every event is machine-recorded with high-resolution epoch timestamps.

However, two major friction points remain:
1. **Approval Prompt Fatigue from Shell/cURL**: When agents or humans initiate or manage collaboration (`/api/collaboration/*`) or execution runs (`/api/exec/*`), using terminal `curl` commands triggers continuous IDE permission prompts, interrupting the workflow.
2. **Missing Statistical & Bottleneck Telemetry**: Operators cannot see where time was spent (Executor on-board vs Reviewer reasoning vs human interlock gating vs reboot recovery), what the command reliability rate was, or how much AI quota (Cursor fast requests / Antigravity credits) was consumed.

This plan implements:
- **Zero-cURL MCP Tools**: Full lifecycle orchestration and execution controls directly exposed as first-class native MCP tools so agents never need shell `curl` or terminal popups.
- **Execution Run Metrics & Waterfall Telemetry**: High-resolution latency analytics, command reliability KPIs, quota delta attribution, and interactive dashboard waterfall timeline.

---

## 2. Architecture Overview

```
                   Agent (Cursor or Gemini)
                          |
                          | Native MCP Tools (NO cURL / Shell Prompts):
                          | - collab_start, collab_run, collab_step, collab_get_status, collab_abort
                          | - exec_start_run, exec_submit_checkpoint, exec_review_checkpoint
                          | - exec_interlock_wait, exec_end_run, exec_get_run_metrics, exec_list_runs
                          v
    +-----------------------------------------------------+
    |                    aimon daemon                      |
    |                                                     |
    |  TranscriptSink         StateStore (Quotas)         |
    |  - events (timestamps)  - Antigravity quota         |
    |  - exit codes, commands - Cursor fast requests      |
    |          |                      |                   |
    |          +----------+-----------+                   |
    |                     v                               |
    |           RunMetricsAnalyzer                        |
    |           - Phase latencies & waterfall breakdown   |
    |           - Human gating ratio & autonomous %       |
    |           - Command reliability & recovery index    |
    |           - AI quota delta attribution              |
    |                                                     |
    |  CollabOrchestrator     InterlockManager            |
    |  - Headless proxies     - CV-wait per run           |
    |  - Auto-drive sessions  - Approve/Reject from UI    |
    |                                                     |
    |  WebServer                                          |
    |  - REST & SSE mirrors for dashboard                 |
    +-----------------------------------------------------+
             |
             | Dashboard: Waterfall Timeline, KPIs & Approve/Reject
             v
         Human (browser at localhost:3883)
```

### Frozen decisions (Turn 1, live MCP probe)

These are locked from connecting to the running daemon; Gemini should treat
them as requirements, not open questions.

1. **Exec tools already registered**: `exec_start_run`,
   `exec_submit_checkpoint`, `exec_review_checkpoint`,
   `exec_interlock_wait`, `exec_get_transcript`. Do not re-spec these;
   only add the missing three (`exec_end_run`, `exec_list_runs`,
   `exec_get_run_metrics`).
2. **Collab lifecycle MCP tools are absent**. The catalog has
   `agent_collaborate` and `agent_wait_turn` only. Kickoff still requires
   the dashboard or `aimon collab <plan_file>` until `collab_start` /
   `collab_get_status` / `collab_abort` / `collab_step` / `collab_run`
   exist. Agents must not use shell `curl` for those once the MCP tools
   ship.
3. **Interlock path is proven**. Run `20260917-111001-cfc3` started via
   MCP, reviewed with `decision: wait_human`, and the operator Approved
   on the dashboard (`interlock_resolved`).
4. **Plan paths stay repository-relative** (`plan/plan_….md`). Do not
   persist absolute home paths in session metadata or docs.
5. **Dashboard verification** uses `http://localhost:3883`, not a
   private cluster hostname.

---

## 3. Native MCP Tool Suite (Zero-cURL)

To permanently eliminate shell `curl` commands and approval popups, the following native MCP tools will be registered in `aimon`:

### 3.1 Collaboration Lifecycle Tools

1. **`collab_start`**:
   - Initiates a document-centric collaboration session.
   - Parameters:
     - `plan_file` (string, required): Path to plan document.
     - `initiator_id` (string, optional, default: `"agent-antigravity-builder"`).
     - `reviewer_id` (string, optional, default: `"agent-cursor-windows"`).
     - `max_turns` (integer, optional, default: 8).
   - Replaces `curl -X POST /api/collaboration/start`.

2. **`collab_run`**:
   - Instructs `aimon` to autonomously drive the session turn-by-turn until consensus.
   - Parameters:
     - `plan_file` (string, required).
     - `initiator_id` (string, optional).
     - `reviewer_id` (string, optional).
     - `max_turns` (integer, optional).
   - Replaces `curl -X POST /api/collaboration/run` and `aimon collab run`.

3. **`collab_step`**:
   - Manually steps a single turn in the collaboration orchestrator.
   - Replaces `curl -X POST /api/collaboration/step` and `aimon collab step`.

4. **`collab_get_status`**:
   - Returns the active collaboration session status, current turn, next actor, and orchestrator state.
   - Replaces `curl -s /api/collaboration/status` and `aimon collab status`.

5. **`collab_abort`**:
   - Aborts active collaboration and halts running proxies.
   - Replaces `curl -X POST /api/collaboration/abort` and `aimon collab abort`.

### 3.2 Execution & Metrics Lifecycle Tools

6. **`exec_end_run`**:
   - Concludes an active execution run and updates `run.json` and `report.md`.
   - Parameters:
     - `run_id` (string, optional, default: active run).
     - `terminal_status` (string, optional, enum: `["completed", "aborted", "failed"]`, default: `"completed"`).
   - Replaces `curl -X POST /api/exec/runs/:id/end`.

7. **`exec_list_runs`**:
   - Lists all execution runs with metadata and indicates the active run ID.
   - Replaces `curl -s /api/exec/runs`.

8. **`exec_get_run_metrics`**:
   - Calculates and returns phase latencies, waterfall segments, reliability stats, and quota attribution deltas.
   - Parameters:
     - `run_id` (string, optional, default: active run).
   - Replaces `curl -s /api/exec/runs/:id/metrics`.

---

## 4. Telemetry Data Models & Metrics Engine

### 4.1 Data Models

In [include/RunMetrics.hxx](file:///home/samurai/work/aimon/include/RunMetrics.hxx):

```cpp
struct QuotaDelta {
    int cursorFastRequestsDelta = 0;
    double cursorSpendUsdDelta = 0.0;
    int antigravityPromptCreditsDelta = 0;
    int antigravityFlowCreditsDelta = 0;

    nlohmann::json toJson() const;
    static QuotaDelta fromJson(const nlohmann::json& j);
};

struct PhaseSegment {
    int seq = 0;
    std::string checkpointId;
    std::string phase;        // "execution", "review", "interlock", "recovery", "setup"
    std::string actor;        // "gemini", "cursor", "human", "aimon"
    int64_t startEpoch = 0;
    int64_t endEpoch = 0;
    int durationSeconds = 0;
    std::string summary;

    nlohmann::json toJson() const;
};

struct RunMetrics {
    std::string runId;
    int64_t totalDurationSeconds = 0;
    int64_t executionDurationSeconds = 0;   // On-target commands + diagnostics
    int64_t reviewDurationSeconds = 0;      // Model review and reasoning
    int64_t interlockDurationSeconds = 0;   // Human gating wait time
    int64_t recoveryDurationSeconds = 0;    // Reboot / recovery time
    
    double humanGatingRatio = 0.0;          // interlockDuration / totalDuration
    double autonomousRatio = 0.0;           // (execution + review) / totalDuration

    int totalCheckpoints = 0;
    int recoveryCount = 0;
    int totalCommands = 0;
    int successfulCommands = 0;
    int failedCommands = 0;
    double commandSuccessRate = 0.0;

    QuotaDelta quotaDelta;
    std::vector<PhaseSegment> waterfall;

    nlohmann::json toJson() const;
};
```

### 4.2 Analytics Engine: `RunMetricsAnalyzer`

In [src/RunMetrics.cxx](file:///home/samurai/work/aimon/src/RunMetrics.cxx):
- Reconstructs exact waterfall phases from `TranscriptEvent` sequences.
- Accurately clocks human gating duration (`interlock` $\to$ `interlock_resolved`).
- Clocks executor time (`previous` $\to$ `result`).
- Clocks reviewer time (`result` $\to$ `review`).
- Clocks recovery loops (`recover` decision $\to$ next healthy checkpoint).
- Computes quota deltas from initial snapshot stored in `RunMetadata`.

---

## 5. Dashboard UI Enhancements

In [web/index.html](file:///home/samurai/work/aimon/web/index.html) and [web/app.js](file:///home/samurai/work/aimon/web/app.js):

1. **Executive KPI Bar**:
   - Autonomous execution % vs Human gating wait %.
   - Command reliability (e.g. `8/9 passed (89%)`).
   - AI Quota burned for this specific run.
2. **Segmented Waterfall Timeline**:
   - Interactive colored horizontal timeline:
     - **Green**: Execution
     - **Purple**: Reviewer analysis
     - **Amber**: Human interlock wait
     - **Orange**: Recovery / reboot loops
   - Tooltips on hover showing duration, checkpoint ID, and commands.

---

## 6. Verification Plan

### Automated Tests
1. **Zero-cURL MCP Verification**:
   - Call `collab_start`, `collab_get_status`, `collab_abort` strictly via MCP client (0 shell / curl calls).
   - Call `exec_start_run`, `exec_end_run`, `exec_list_runs`, `exec_get_run_metrics` strictly via MCP client.
   - Verify all return clean structured JSON results.
2. **Metrics & Waterfall Accuracy**:
   - Replay iterations 5–10 fixture dataset.
   - Verify `exec_get_run_metrics` computes correct checkpoint count (6), recovery count (1), and command rates.
   - Verify waterfall segments sum to total active time.
3. **Dashboard Visual**:
   - Inspect `http://localhost:3883` to verify waterfall bar and KPI cards.

---

## License & Copyright

Copyright (C) 2026, Charles Chiou. All rights reserved.

---

## Collaboration

### Turn 1: Cursor Grok 4.6 (Initiator)

- **Turn Started**: 2026-09-17T11:15:13+08:00
- **Model**: Cursor Grok 4.6
- **Role**: Initiator
- **Re-work & Summary**:
  Connected via `agent_wait_turn` after kickoff. Live catalog probe on
  the daemon showed exec I1/I2 tools already present and collab
  lifecycle MCP tools missing, so the plan body now freezes that
  inventory, drops the private dashboard hostname from verification,
  and records that run `20260917-111001-cfc3` already exercised
  `wait_human` plus operator Approve. Reviewer should critique whether
  `collab_run` auto-drive belongs in the same slice as metrics, or
  should stay dashboard/CLI until the zero-curl tools exist.
- **Status**: IN_PROGRESS
- **Turn Finished**: 2026-09-17T11:17:05+08:00

### Turn 2: agent-antigravity-builder (Reviewer)
- **Turn Started**: 2026-09-17T11:16:16+08:00
- **Model**: agent-antigravity-builder
- **Role**: Reviewer
- **Feedback & Critique**:
  
Here is the architectural critique and actionable feedback for the initiator:

1. **Scope Management (`collab_*` vs `exec_*`)**: Addressing your question regarding `collab_run`—bundling the entire `collab_*` macro lifecycle suite (start, run, step, abort) in the same slice as the Execution Metrics and Waterfall telemetry is too broad and introduces unnecessary risk. The `collab_*` tools should be decoupled into a separate orchestration plan. Keep this plan tightly scoped to the `exec_*` MCP tools, telemetry models, and dashboard metrics.

2. **In-Progress Run Handling**: The plan does not specify how `RunMetricsAnalyzer` calculates metrics for an *active* run. If `exec_get_run_metrics` is called while a run is ongoing, the current `PhaseSegment` will not have an `endEpoch`. The plan must define how active segments are calculated (e.g., using the current epoch time for the latest phase) and whether the dashboard gracefully handles incomplete phases.

3. **Quota Snapshot Sourcing**: The plan mentions computing "quota deltas from initial snapshot stored in RunMetadata," but the mechanism for capturing this snapshot is missing. You need to explicitly define how and when `aimon` pulls the initial quota from `StateStore` at run start and how it is structured within `RunMetadata` to ensure `QuotaDelta` computation is reliable. 

4. **MCP Tool Error Signatures**: The new `exec_*` MCP tools must define their error handling behaviors. For instance, what is the exact JSON error structure returned if `exec_get_run_metrics` receives an invalid or non-existent `run_id`?

Please revise the plan to narrow the scope and address these edge cases.

- **Status**: IN_PROGRESS
- **Turn Finished**: 2026-09-17T11:17:15+08:00
