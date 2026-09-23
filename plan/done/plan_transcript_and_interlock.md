# Transcript Sink & Interlock UI

## 1. Problem Statement

During the N1-655 camera bring-up, the human sat between two IDE agents
copying reports and instructions back and forth. The entire session was
recorded only as unstructured markdown appended to `gemini_stuck_report.md`.
There is no machine-parseable event log, no structured timeline, and no way
to replay or analyze the 8 iterations that kept the board recoverable.

This plan implements the first two iterations from `TODO.md` §174–192:

1. **Transcript Sink** — structured JSONL event recording with a markdown
   mirror, enabling post-session analysis without any agent dispatch
   changes.
2. **Interlock UI** — dashboard Approve/Reject buttons wired to a blocking
   MCP tool, so the human clicks instead of copy-pasting.

These two iterations remove the human from logging (iteration 1) and from
message relay for gated operations (iteration 2). They require zero changes
to `AgentRunner` or `CollabOrchestrator` proxy execution.

---

## 2. Architecture Overview

```
                   Agent (Cursor or Gemini)
                          |
                          | MCP: exec_submit_checkpoint / exec_review_checkpoint
                          v
   +-----------------------------------------------------+
   |                    aimon daemon                      |
   |                                                     |
   |  TranscriptSink        InterlockManager             |
   |  - runs/<id>/run.json  - CV-wait per run            |
   |  - runs/<id>/tx.jsonl  - Approve/Reject from UI     |
   |  - runs/<id>/report.md - reason string on Reject    |
   |                                                     |
   |  WebServer                                          |
   |  - GET  /api/exec/runs                              |
   |  - GET  /api/exec/runs/<id>/transcript              |
   |  - POST /api/exec/runs/<id>/interlock  (dashboard)  |
   |  - SSE  interlock_request / interlock_resolved       |
   |                                                     |
   |  McpServer (new exec_* MCP tools)                   |
   |  - exec_start_run                                   |
   |  - exec_submit_checkpoint                           |
   |  - exec_review_checkpoint                           |
   |  - exec_interlock_wait                              |
   |  - exec_get_transcript                              |
   +-----------------------------------------------------+
            |
            | Dashboard: Approve / Reject / Abort
            v
        Human (browser at localhost:3883)
```

---

## 3. Iteration 1: Transcript Sink

### 3.1 On-Disk Layout

Each execution run creates a directory under the aimon data directory:

```
~/.config/aimon/runs/<run-id>/
  run.json          # Run metadata (plan path, actors, policy, start/end)
  transcript.jsonl  # One JSON object per event, append-only
  report.md         # Human-readable markdown mirror, auto-generated
```

`<run-id>` is a compact timestamp + short random suffix, e.g.
`20260917-103400-a3f2`. The `runs/` directory is created on first use.

### 3.2 New Class: `TranscriptSink`

#### [NEW] `include/TranscriptSink.hxx`

```cpp
struct RunMetadata {
    std::string runId;
    std::string planFile;
    std::string workspace;
    std::string targetAlias;       // e.g. "n1-655-devkit"
    std::string initiatorId;       // e.g. "agent-cursor-windows"
    std::string executorId;        // e.g. "agent-antigravity-builder"
    std::string policyId;          // reserved, empty for now
    int64_t startEpoch = 0;
    int64_t endEpoch = 0;
    std::string terminalStatus;    // "completed", "aborted", "failed"
};

struct TranscriptEvent {
    int64_t tsEpoch = 0;
    std::string runId;
    int seq = 0;
    std::string actor;             // "cursor" | "gemini" | "human" | "aimon"
    std::string kind;              // see below
    std::string checkpointId;
    nlohmann::json commands;       // ["insmod dsp.ko", ...]
    nlohmann::json exitCodes;      // [0, -1, ...]
    nlohmann::json artifacts;      // {"dtb_hash": "abc123", ...}
    std::string dmesgExcerpt;
    std::string decision;          // "continue"|"stop"|"recover"|"wait_human"
    std::string proposalNext;
    std::string notes;             // free-form text
};
```

Event `kind` values: `dispatch`, `command`, `result`, `review`,
`interlock`, `interlock_resolved`, `policy_deny`, `recover`, `abort`,
`start`, `end`.

#### Methods

- `startRun(RunMetadata)` — creates `runs/<id>/`, writes `run.json`,
  emits `start` event.
- `appendEvent(TranscriptEvent)` — appends one JSON line to
  `transcript.jsonl`, updates `report.md` mirror. Thread-safe via mutex.
- `endRun(terminalStatus)` — emits `end` event, updates `run.json`
  with `endEpoch` and `terminalStatus`.
- `getRunMetadata(runId)` → `RunMetadata`.
- `getTranscript(runId, sinceSeq)` → `vector<TranscriptEvent>`.
- `listRuns()` → `vector<RunMetadata>` (scans `runs/` directory).

#### [NEW] `src/TranscriptSink.cxx`

Implementation details:
- `transcript.jsonl` is opened in append mode (`std::ios::app`).
  Each `appendEvent` call writes one line and flushes.
- `report.md` is regenerated from `transcript.jsonl` on each append
  (or maintained incrementally by appending a markdown section).
- `seq` is auto-incremented per run, starting at 1.
- `tsEpoch` is set automatically if zero.

### 3.3 MCP Tools (Transcript)

#### [NEW] `exec_start_run`

```json
{
  "name": "exec_start_run",
  "parameters": {
    "properties": {
      "plan_file": { "type": "string" },
      "workspace": { "type": "string" },
      "target_alias": { "type": "string" },
      "executor_id": { "type": "string" }
    },
    "required": ["plan_file", "target_alias"]
  }
}
```

Returns `{ "run_id": "...", "status": "started" }`.

Only one run may be active at a time (enforced by `TranscriptSink`).

#### [NEW] `exec_submit_checkpoint`

Called by the Executor (Gemini) after completing one checkpoint.

```json
{
  "name": "exec_submit_checkpoint",
  "parameters": {
    "properties": {
      "run_id": { "type": "string" },
      "checkpoint_id": { "type": "string" },
      "commands": { "type": "array", "items": { "type": "string" } },
      "exit_codes": { "type": "array", "items": { "type": "integer" } },
      "dmesg_excerpt": { "type": "string" },
      "artifacts": { "type": "object" },
      "notes": { "type": "string" }
    },
    "required": ["run_id", "checkpoint_id"]
  }
}
```

Writes a `result` event to the transcript. Returns the event `seq`.

#### [NEW] `exec_review_checkpoint`

Called by the Reviewer (Cursor) after inspecting a checkpoint result.

```json
{
  "name": "exec_review_checkpoint",
  "parameters": {
    "properties": {
      "run_id": { "type": "string" },
      "seq": { "type": "integer" },
      "decision": {
        "type": "string",
        "enum": ["continue", "stop", "recover", "wait_human"]
      },
      "next_instructions": { "type": "string" },
      "notes": { "type": "string" }
    },
    "required": ["run_id", "decision"]
  }
}
```

Writes a `review` event. If `decision` is `wait_human`, also creates an
interlock request (see iteration 2).

#### [NEW] `exec_get_transcript`

```json
{
  "name": "exec_get_transcript",
  "parameters": {
    "properties": {
      "run_id": { "type": "string" },
      "since_seq": { "type": "integer" }
    },
    "required": ["run_id"]
  }
}
```

Returns transcript events as a JSON array, optionally filtered by
`since_seq` for incremental polling.

### 3.4 Fixture Replay

Before wiring MCP tools, validate `TranscriptSink` by replaying the
N1-655 `gemini_stuck_report.md` iterations 5–10 as fixture events:

1. Parse the 6 iterations into `TranscriptEvent` structs manually
   in a unit test or script.
2. Call `startRun` + `appendEvent` × N + `endRun`.
3. Verify `transcript.jsonl` round-trips correctly via
   `getTranscript`.
4. Verify `report.md` is a readable mirror.

---

## 4. Iteration 2: Interlock UI

### 4.1 New Class: `InterlockManager`

#### [NEW] `include/InterlockManager.hxx`

```cpp
struct InterlockRequest {
    std::string runId;
    std::string checkpointId;
    int seq = 0;
    std::string description;       // What is being gated
    std::string proposedAction;    // What the agent wants to do
    int64_t requestedEpoch = 0;
    // Resolution (filled when human responds)
    bool resolved = false;
    bool approved = false;
    std::string rejectReason;
    int64_t resolvedEpoch = 0;
};

class InterlockManager {
public:
    static InterlockManager& getInstance();

    // Creates an interlock and notifies dashboard via SSE.
    std::string createInterlock(const std::string& runId,
                                const std::string& checkpointId,
                                int seq,
                                const std::string& description,
                                const std::string& proposedAction);

    // Blocks until the human clicks Approve or Reject on the
    // dashboard (or timeout). Returns the resolution.
    bool waitInterlock(const std::string& interlockId,
                       int timeoutSeconds,
                       InterlockRequest& outResult);

    // Called by the dashboard REST endpoint.
    bool resolveInterlock(const std::string& interlockId,
                          bool approved,
                          const std::string& rejectReason);

    InterlockRequest getInterlock(const std::string& interlockId) const;
    std::vector<InterlockRequest> getPendingInterlocks() const;

private:
    mutable std::mutex _mutex;
    std::condition_variable _cv;
    std::map<std::string, InterlockRequest> _interlocks;
};
```

The blocking wait uses the same `condition_variable` pattern as
`AgentMessageBus::waitCollaborationTurn` — proven in 3 live sessions.

### 4.2 MCP Tool: `exec_interlock_wait`

```json
{
  "name": "exec_interlock_wait",
  "parameters": {
    "properties": {
      "run_id": { "type": "string" },
      "timeout_seconds": { "type": "integer" }
    },
    "required": ["run_id"]
  }
}
```

Called by an agent that has been told `decision: wait_human`. Blocks
until the human responds on the dashboard. Returns:

```json
{
  "approved": true,
  "reject_reason": ""
}
```

or

```json
{
  "approved": false,
  "reject_reason": "DTB change not safe without serial console attached"
}
```

### 4.3 REST Endpoints (Dashboard)

#### [MODIFY] `src/WebServer.cxx`

Add under `/api/exec/`:

| Method | Path | Purpose |
| :--- | :--- | :--- |
| GET | `/api/exec/runs` | List all runs (metadata only) |
| GET | `/api/exec/runs/<id>/transcript` | Get transcript events |
| GET | `/api/exec/interlocks` | List pending interlocks |
| POST | `/api/exec/interlocks/<id>/resolve` | Approve or Reject |

The resolve endpoint accepts:

```json
{ "approved": true }
```

or

```json
{ "approved": false, "reason": "..." }
```

### 4.4 Dashboard UI

#### [MODIFY] `web/index.html`, `web/app.js`

Add an "Execution Runs" panel below the existing collaboration status:

- **Active Run**: run ID, plan file, target alias, checkpoint count,
  elapsed time.
- **Pending Interlock** (if any): yellow banner with description,
  proposed action, and two buttons: **Approve** (green) / **Reject**
  (red, opens a reason text field).
- **Recent Transcript**: scrollable list of the last N events,
  color-coded by `kind` (dispatch = blue, result = green,
  interlock = yellow, recover = orange, stop = red).

SSE events `interlock_request` and `interlock_resolved` update the
banner in real time without polling.

---

## 5. Proposed Changes Summary

### TranscriptSink Component

#### [NEW] `include/TranscriptSink.hxx`
- `RunMetadata`, `TranscriptEvent` structs
- `TranscriptSink` class with `startRun`, `appendEvent`, `endRun`,
  `getTranscript`, `listRuns`

#### [NEW] `src/TranscriptSink.cxx`
- JSONL append-only file writer, markdown mirror generator
- Auto-seq, auto-timestamp, thread-safe via mutex

---

### InterlockManager Component

#### [NEW] `include/InterlockManager.hxx`
- `InterlockRequest` struct, `InterlockManager` singleton class

#### [NEW] `src/InterlockManager.cxx`
- CV-based blocking wait, SSE notification on create/resolve

---

### MCP Integration

#### [MODIFY] `src/McpServer.cxx`
- Register 5 new MCP tools: `exec_start_run`, `exec_submit_checkpoint`,
  `exec_review_checkpoint`, `exec_interlock_wait`, `exec_get_transcript`
- Tool handlers delegate to `TranscriptSink` and `InterlockManager`

#### [NEW] MCP schema files (5 files under project root for discovery)
- `exec_start_run.json`, `exec_submit_checkpoint.json`,
  `exec_review_checkpoint.json`, `exec_interlock_wait.json`,
  `exec_get_transcript.json`

---

### Web/Dashboard

#### [MODIFY] `src/WebServer.cxx`
- 4 new REST endpoints under `/api/exec/`
- SSE events for interlock lifecycle

#### [MODIFY] `web/index.html` and `web/app.js`
- Execution Runs panel, interlock banner, transcript viewer

---

### Build

#### [MODIFY] `CMakeLists.txt`
- Add `src/TranscriptSink.cxx` and `src/InterlockManager.cxx` to
  sources

#### [MODIFY] `src/Main.cxx`
- Instantiate `TranscriptSink` and `InterlockManager` singletons
- Wire into `WebServer` and `McpServer`

---

## 6. What Is NOT Changed

- `CollabOrchestrator` — no changes. The existing plan-document
  collaboration mode continues to work independently.
- `AgentRunner` — no changes. Agent dispatch is a future iteration.
- `AgentMessageBus` — no changes. The exec namespace is separate from
  the collab namespace.
- Existing MCP tools (`agent_collaborate`, `agent_wait_turn`, etc.) —
  untouched.

---

## 7. Verification Plan

### Automated Tests

1. **Transcript Round-Trip**:
   - `startRun` → 6 × `appendEvent` → `endRun`
   - `getTranscript` returns all 6 events with correct seq numbers
   - `transcript.jsonl` is valid JSONL (each line parses independently)
   - `report.md` contains all checkpoint summaries

2. **Interlock Lifecycle**:
   - `createInterlock` → `waitInterlock` (background thread) →
     `resolveInterlock(approved=true)` → wait returns `approved=true`
   - `createInterlock` → `resolveInterlock(approved=false, reason=...)` →
     wait returns `approved=false` with reason
   - Timeout: `waitInterlock(timeout=2)` returns timeout error after 2s

3. **MCP Tool Integration**:
   - Call `exec_start_run` via curl → verify `run.json` created
   - Call `exec_submit_checkpoint` → verify `transcript.jsonl` has entry
   - Call `exec_review_checkpoint(decision=wait_human)` → verify
     interlock created
   - Call `/api/exec/interlocks/<id>/resolve` → verify
     `exec_interlock_wait` unblocks

### Manual Verification

4. **Dashboard Visual**:
   - Open `localhost:3883` → verify "Execution Runs" panel renders
   - Create an interlock → verify yellow banner appears with
     Approve/Reject buttons
   - Click Approve → verify banner disappears and SSE event fires

5. **Build Verification**:
   - `make` compiles cleanly on `builder`
   - `./build/aimon daemon` starts without errors

---

## License & Copyright

Copyright (C) 2026, Charles Chiou. All rights reserved.
