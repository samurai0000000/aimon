# Document-Centric Inter-Agent Collaboration Architecture & Tool Specification

## 1. Introduction & Foundational Reference

The following instructions from the user define the requirements, operational model, and ultimate reference for this system:

> "I will first create a plan with Gemini, which is saved under plan/plan_new_feature.md  
> I will then initiate a collaboration using aimon's new tool for Gemini and Cursor to refine this plan until there's consensus between the two parties.  
> Each turn, they will append to the document under the section: "collaboration".  
> Under this section, the next agent in turn will:  
> 1. emit the timestamp he picked up his turn  
> 2. emit his model (e.g., Gemini 3.8 Flash High)  
> 3. Output the critique or feedback  
> 4. emit the timestamp he finished his turn  
>   
> The agent which initiated the plan (odd # turn) can re-work the plan  
> The agent which acts as the reviewer can only critique  
> aimon tool is used to transmit side-channel messages and as a signal to wake up the other agent for his turn  
>   
> The process is finished when consensus is reached and the agents return to normal prompt mode"

---

## 2. Protocol Lineage & Explicit Replacement of Protocol v2

**Explicit Replacement**:  
This document-centric collaboration architecture **replaces** the prior Protocol v2 machine-envelope protocol (`plan_inter_agent_protocol_v2.md`) for all collaborative plan authoring and refinement. 
- Protocol v2's heavy JSON message bus envelopes (`PROPOSAL`, `CRITIQUE`, `AMENDMENT`, `VOTE_REQUEST`, `VOTE`, `HALT`), voting ballots, and multi-turn state machine are retired into historical status.
- Inter-agent collaboration for plans does **NOT** use `agent_check_inbox` or `agent_send_reply`. It operates strictly through two dedicated, purpose-built MCP tools: `agent_collaborate` (doorbell & turn completion) and `agent_wait_turn` (latched turn receiver).
- The file on disk is the authoritative artifact. `aimon` acts strictly as an out-of-band doorbell, latch coordinator, and enforcement gate.

---

## 3. Core Architectural Rules & Enforcement Guarantees

### 3.1 Document-Centric Canvas
1. The target markdown file on disk (e.g., `plan/plan_new_feature.md`) is the single source of truth. Both agents (Linux Antigravity host and Windows Cursor host) interact directly with this file.
2. The document is divided into two distinct zones:
   - **Plan Body**: The text preceding the first markdown heading whose line content is exactly `## Collaboration`.
   - **Collaboration Log**: The append-only section starting with `## Collaboration`.

### 3.2 Role Asymmetry & Handoff Hash Check
Role asymmetry is enforced via a cryptographic handoff check when turns are submitted:
1. **Initiator (Odd Turns: 1, 3, 5, 7)**:
   - Authorized to re-work and edit the **Plan Body** to incorporate reviewer feedback.
   - Appends a turn entry summarizing revisions to `## Collaboration`.
   - When calling `agent_collaborate(turn_number=N)`, `aimon` normalizes newlines and records the new SHA-256 checksum of the Plan Body (`planBodyHash`).
2. **Reviewer (Even Turns: 2, 4, 6, 8)**:
   - **Read-Only on Plan Body**: Permitted strictly to append critique and feedback to `## Collaboration`.
   - **Handoff Hash Check**: When the reviewer calls `agent_collaborate(turn_number=N)`, `aimon` extracts the Plan Body, normalizes newlines (LF), calculates its SHA-256 hash, and compares it to `planBodyHash` recorded at the end of the previous odd turn.
   - **Hard Rejection**: If the hash differs, `agent_collaborate` rejects the turn call with a hard error: `COLLABORATION_ERROR: Reviewer is forbidden from modifying the plan body. Plan body SHA-256 mismatch.`
3. **Cross-Platform CRLF/LF Normalization**:
   To prevent false rejections caused by Windows (CRLF) vs. Linux (LF) line-ending conversions across shared network mounts or git checkouts:
   - Substring extraction takes all bytes prior to the first line matching `^## Collaboration\s*$`.
   - All CRLF (`\r\n`) and CR (`\r`) occurrences are normalized to LF (`\n`).
   - SHA-256 is computed over the resulting normalized UTF-8 string.
   - The identical normalization applies at operator kickoff, odd-turn recording, and even-turn validation.

### 3.3 Unified Status Enum & Authoritative Latch
The daemon maintains exactly one unified state enum across internal structs, REST APIs, and tool responses:
```cpp
enum class CollaborationStatus {
    IDLE,
    IN_PROGRESS,
    CONSENSUS_REACHED,
    OPERATOR_REVIEW
};
```
1. `CollaborationSession` in `aimon` is the **single authoritative source of truth** for turn progression, wakeup latches, and session lifecycle. The markdown log is human-readable documentation and audit history.
2. **One Active Session per Daemon**: `aimon` manages at most **one active collaboration session** at any time. Calling kickoff while another session is `IN_PROGRESS` or `OPERATOR_REVIEW` fails immediately with `COLLABORATION_ERROR: A collaboration session is already active for <existing_plan_file>. Abort or finish it before starting a new one.`
3. **Participant Authorization**: Only registered participants (`initiatorId` or `reviewerId`) may call `agent_collaborate` or `agent_wait_turn`. Calls from unregistered agents fail immediately with `COLLABORATION_ERROR: Agent <agent_id> is not a participant in this collaboration session.`
4. **Consensus Authority**:
   - `status = "CONSENSUS_REACHED"` may **only** be submitted on an **even turn** (2, 4, 6, or 8) by the Reviewer.
   - If an initiator attempts to submit `CONSENSUS_REACHED` on an odd turn, `agent_collaborate` rejects it with: `COLLABORATION_ERROR: Only the designated reviewer can signal CONSENSUS_REACHED on an even turn.`
5. **Terminal Consensus Handoff**:
   - When the reviewer calls `agent_collaborate` with `status = "CONSENSUS_REACHED"` on an even turn:
     - `session.status` transitions directly to `CollaborationStatus::CONSENSUS_REACHED`.
     - `session.nextActorId` is set to `"NONE"`.
     - Record `session.currentTurn = turn_number`; do not schedule another turn.
     - Broadcast SSE event `event: collaboration_turn` to notify connected clients.
     - The call to `agent_collaborate` immediately returns:
       ```json
       {
         "status": "consensus_reached",
         "final_turn": turn_number,
         "message": "Consensus reached. Return to normal prompt mode."
       }
       ```
     - All waiting peers blocked on `agent_wait_turn` are woken via `_collabCv.notify_all()`. They receive `status: "consensus_reached"` and also cleanly exit to normal prompt mode.

### 3.4 Bounded Turn Cap & Operator Escalation
1. Maximum turns are strictly capped at **8 turns** (4 full cycles of Initiator Re-work $\leftrightarrow$ Reviewer Critique).
2. If Turn 8 completes with `status == "IN_PROGRESS"`:
   - `session.status` transitions immediately to `OPERATOR_REVIEW`.
   - `session.nextActorId` is set to `"NONE"`.
   - Peer wakeup is suppressed; no turn 9 is latched.
   - Console status indicator turns red and operator alert is raised.

---

## 4. Document Contract & Validation

Before `agent_collaborate` accepts a turn completion and rings the peer's doorbell, `aimon` parses the target plan file from disk and validates the **Four-Line Document Contract**:

```markdown
## Collaboration

### Turn <N>: <Agent Identifier> (<Role>)
- **Turn Started**: <ISO-8601 or YYYY-MM-DDTHH:MM:SS+TZ>
- **Model**: <Model Identifier, e.g. Gemini 3.8 Flash High or Cursor Grok 4.6 High>
- **Role**: <Initiator | Reviewer>
- **Feedback & Critique**: (or **Re-work & Responses** for odd turns)
  <Itemized text>
- **Turn Finished**: <ISO-8601 or YYYY-MM-DDTHH:MM:SS+TZ>
```

### Validation Invariants:
1. `## Collaboration` heading must exist in the file (note: not required at operator kickoff, but mandatory starting with Turn 1).
2. Section `### Turn <N>:` must exist where `<N>` matches `turn_number`.
3. The lines `Turn Started:`, `Model:`, and `Turn Finished:` must be present with non-empty values.
4. If validation fails, `agent_collaborate` returns an immediate error indicating the missing contract element, preventing fraudulent or empty log handoffs.

---

## 5. Tool Specifications & Wakeup Latches

### 5.1 MCP Tool: `agent_collaborate`
Invoked by the active agent upon finishing its turn.

**JSON-RPC Input Schema**:
```json
{
  "type": "object",
  "properties": {
    "plan_file": {
      "type": "string",
      "description": "Relative or workspace path to the target plan (e.g. 'plan/plan_new_feature.md')."
    },
    "turn_number": {
      "type": "integer",
      "description": "The turn number just completed (e.g. 1, 2, 3...)."
    },
    "agent_id": {
      "type": "string",
      "description": "Canonical ID of calling agent ('agent-antigravity-builder' or 'agent-cursor-windows')."
    },
    "agent_model": {
      "type": "string",
      "description": "Model identifier string (e.g. 'Gemini 3.8 Flash High', 'Cursor Grok 4.6 High')."
    },
    "status": {
      "type": "string",
      "enum": ["IN_PROGRESS", "CONSENSUS_REACHED"],
      "description": "Session status ('IN_PROGRESS' or 'CONSENSUS_REACHED')."
    },
    "side_channel_message": {
      "type": "string",
      "description": "Brief note or highlight passed out-of-band to the peer and displayed on operator console."
    }
  },
  "required": ["plan_file", "turn_number", "agent_id", "agent_model", "status"]
}
```

**Daemon Execution Logic**:
1. Verify session is active and `plan_file == session.planFile`.
2. Verify calling `agent_id` is a registered participant (`initiatorId` or `reviewerId`).
3. Verify `agent_id == session.nextActorId`. If not, reject as out-of-turn.
4. Verify `turn_number == session.currentTurn + 1`. If not, reject sequence mismatch.
5. Validate plan file exists on disk and satisfies the Section 4 Document Contract.
6. If even turn (reviewer):
   - Compute normalized SHA-256 of Plan Body (above `## Collaboration`).
   - If mismatch with `session.planBodyHash`, reject with write-lease violation error.
   - If `status == "CONSENSUS_REACHED"`:
     - `session.status = CollaborationStatus::CONSENSUS_REACHED`
     - `session.currentTurn = turn_number`
     - `session.nextActorId = "NONE"`
     - `session.latchedTurnReady = true`
     - `session.lastSideChannelMessage = side_channel_message`
     - Broadcast SSE event `event: collaboration_turn` to streaming clients.
     - `_collabCv.notify_all()`
     - Return: `{"status": "consensus_reached", "final_turn": turn_number, "message": "Consensus reached. Return to normal prompt mode."}`
   - If `status == "IN_PROGRESS"` and `turn_number >= session.maxTurns`:
     - `session.status = CollaborationStatus::OPERATOR_REVIEW`
     - `session.currentTurn = turn_number`
     - `session.nextActorId = "NONE"`
     - `session.latchedTurnReady = false`
     - Broadcast SSE event `event: collaboration_turn` to streaming clients.
     - `_collabCv.notify_all()`
     - Alert operator on console and web dashboard.
     - Return: `{"status": "operator_review", "turn_number": turn_number, "message": "Turn cap reached without consensus. Escalated to operator."}`
7. If odd turn (initiator):
   - Reject if `status == "CONSENSUS_REACHED"` (only reviewer can signal consensus).
   - Update `session.planBodyHash = normalized_sha256(plan_body)`.
8. Normal turn handover (`IN_PROGRESS`, turn < maxTurns):
   - `session.currentTurn = turn_number`
   - `session.nextActorId = (agent_id == session.initiatorId) ? session.reviewerId : session.initiatorId`
   - `session.lastTurnEpoch = now()`
   - `session.lastSideChannelMessage = side_channel_message`
   - `session.latchedTurnReady = true`
   - `_collabCv.notify_all()`
   - Broadcast SSE event `event: collaboration_turn` to streaming clients.
   - Return: `{"status": "turn_signaled", "turn_number": turn_number, "next_actor_id": session.nextActorId}`.

---

### 5.2 MCP Tool: `agent_wait_turn`
Invoked by the waiting agent to receive its turn signal.

**JSON-RPC Input Schema**:
```json
{
  "type": "object",
  "properties": {
    "plan_file": {
      "type": "string",
      "description": "Path to the plan file to wait for (e.g. 'plan/plan_new_feature.md')."
    },
    "agent_id": {
      "type": "string",
      "description": "Canonical ID of calling agent ('agent-antigravity-builder' or 'agent-cursor-windows')."
    },
    "timeout_seconds": {
      "type": "integer",
      "description": "Seconds to wait. 0 for indefinite blocking (kernel cv wait). >0 for polling (default: 0)."
    }
  },
  "required": ["plan_file", "agent_id"]
}
```

**Non-Destructive Latching & Turn Number Formula**:
- **Execution Turn Formula**: The turn number that the agent is expected to execute is strictly defined as:
  $$\text{turn\_number\_to\_execute} = \text{session.currentTurn} + 1$$
  - After kickoff (`currentTurn = 0`), `turn_number_to_execute = 1` (for initiator).
  - After Turn 1 completes (`currentTurn = 1`), `turn_number_to_execute = 2` (for reviewer).
- **Strict Evaluation Order for `agent_wait_turn`**:
  1. **No Session or IDLE**: If no session exists or `session.status == IDLE` $\rightarrow$ return immediately `{"status": "no_session"}` (do not block forever).
  2. **Validation**: If `plan_file != session.planFile` or `agent_id` is neither `initiatorId` nor `reviewerId` $\rightarrow$ return `COLLABORATION_ERROR`.
  3. **Terminal Consensus**: If `session.status == CollaborationStatus::CONSENSUS_REACHED` $\rightarrow$ return immediately `{"status": "consensus_reached", "final_turn": session.currentTurn, "collaboration_status": "CONSENSUS_REACHED", "side_channel_message": session.lastSideChannelMessage}`.
  4. **Operator Review Unblock**: If `session.status == CollaborationStatus::OPERATOR_REVIEW` $\rightarrow$ return immediately `{"status": "operator_review", "current_turn": session.currentTurn, "collaboration_status": "OPERATOR_REVIEW", "message": "Turn cap reached. Session escalated to operator."}`.
  5. **Turn Ready**: Else if `session.nextActorId == agent_id && session.latchedTurnReady` $\rightarrow$ return immediately:
     ```json
     {
       "status": "turn_ready",
       "plan_file": session.planFile,
       "turn_number": session.currentTurn + 1,
       "role": (agent_id == session.initiatorId) ? "initiator" : "reviewer",
       "prior_agent": (agent_id == session.initiatorId) ? session.reviewerId : session.initiatorId,
       "prior_model": session.lastModel,
       "collaboration_status": "IN_PROGRESS",
       "side_channel_message": session.lastSideChannelMessage,
       "file_mtime_epoch": session.lastFileMtime,
       "instruction": "Re-read " + session.planFile + " fresh from disk before beginning your turn."
     }
     ```
  6. **Wait or Short-Poll**: Else (`session.status == CollaborationStatus::IN_PROGRESS` but not this agent's turn):
     - If `timeout_seconds == 0`: Wait on `_collabCv` condition variable until state changes, then re-evaluate from Step 1.
     - If `timeout_seconds > 0`: Wait on `_collabCv` for up to `timeout_seconds`. If timed out, return `{"status": "idle", "current_turn": session.currentTurn, "next_actor_id": session.nextActorId}` without modifying the latch.
- **Turn Ready Output**:
  When it is the calling agent's turn (`session.nextActorId == agent_id` and `session.latchedTurnReady`):
  ```json
  {
    "status": "turn_ready",
    "plan_file": "plan/plan_new_feature.md",
    "turn_number": 2,
    "role": "reviewer",
    "prior_agent": "agent-antigravity-builder",
    "prior_model": "Gemini 3.8 Flash High",
    "collaboration_status": "IN_PROGRESS",
    "side_channel_message": "Turn 1 plan draft ready for critique.",
    "file_mtime_epoch": 1789568200,
    "instruction": "Re-read plan/plan_new_feature.md fresh from disk before beginning your turn."
  }
  ```
- **Consensus Terminal Output**:
  When `session.status == CollaborationStatus::CONSENSUS_REACHED`:
  ```json
  {
    "status": "consensus_reached",
    "plan_file": "plan/plan_new_feature.md",
    "final_turn": 4,
    "collaboration_status": "CONSENSUS_REACHED",
    "side_channel_message": "Consensus reached. Return to normal prompt mode."
  }
  ```

---

## 6. Operator Kickoff & Anti-Race Mechanics

### 6.1 Kickoff Command (`collab` / `collaborate`)
The operator initiates collaboration from the CLI or REST API:
```text
aimon> collab plan/plan_new_feature.md agent-antigravity-builder agent-cursor-windows
```
or REST:
```bash
POST /api/collaboration/start
{
  "plan_file": "plan/plan_new_feature.md",
  "initiator_id": "agent-antigravity-builder",
  "reviewer_id": "agent-cursor-windows"
}
```

### 6.2 Kickoff Initialization & Validations (Solving the Wait Race)
1. **Kickoff Validations**:
   - Verify `initiator_id != reviewer_id`.
   - Verify `plan_file` exists on disk (returns error if missing).
   - Verify no existing session is `IN_PROGRESS` or `OPERATOR_REVIEW`.
   - Note: The file is **not** required to contain `## Collaboration` at kickoff; the initiator creates that section during Turn 1.
2. **Session Initialization**:
   - `planFile = "plan/plan_new_feature.md"`
   - `initiatorId = initiator_id`
   - `reviewerId = reviewer_id`
   - `currentTurn = 0`
   - `nextActorId = initiatorId`
   - `latchedTurnReady = true` (Turn 1 is immediately ready for initiator)
   - `status = CollaborationStatus::IN_PROGRESS`
   - `planBodyHash = normalized_sha256(entire_file_content)`
3. Trigger `_collabCv.notify_all()`.
4. If the initiator is already waiting in `agent_wait_turn`, it unblocks immediately. If it calls `agent_wait_turn` subsequent to kickoff, it receives `status: "turn_ready"` for Turn 1 immediately.

### 6.3 Turn SLA & Operator Intervention Clocks
To prevent hangs if an agent crashes or loses network:
- **180s**: Operator console status indicator turns yellow (`Turn N: agent thinking...`).
- **300s**: Daemon logs keep-alive warning and emits SSE heartbeat.
- **600s**: Daemon raises operator alert: `Turn SLA exceeded by <nextActorId>`.
- **Operator Commands**:
  - `collab nudge`: Re-triggers condition variable and emits SSE event.
  - `collab takeover [agent_id]`: Operator overrides turn or assigns `nextActorId`.
  - `collab abort`: Terminates the collaboration session cleanly, setting `status = IDLE`.

---

## 7. Component Modifications in `aimon`

### 7.1 `AgentMessageBus` (`include/AgentMessageBus.hxx`, `src/AgentMessageBus.cxx`)
- Define `CollaborationSession` struct:
  ```cpp
  struct CollaborationSession {
      std::string planFile;
      std::string initiatorId;
      std::string reviewerId;
      std::string nextActorId;
      int currentTurn = 0;
      int maxTurns = 8;
      CollaborationStatus status = CollaborationStatus::IDLE;
      std::string planBodyHash;
      std::string lastSideChannelMessage;
      int64_t lastTurnEpoch = 0;
      bool latchedTurnReady = false;
  };
  ```
- Store a single active `CollaborationSession _activeCollabSession;` guarded by `_mutex`.
- Implement methods:
  - `startCollaboration(...)`
  - `signalCollaborationTurn(...)`
  - `waitCollaborationTurn(...)`
  - `nudgeCollaboration()`
  - `abortCollaboration()`
  - `getCollaborationStatus()`
  - `normalizeNewlinesAndHash(...)`
  - `validateDocumentContract(...)`

### 7.2 `McpServer` (`include/McpServer.hxx`, `src/McpServer.cxx`)
- Register tools in `tools/list`:
  - `agent_collaborate`
  - `agent_wait_turn`
- Implement handlers with full validation checks:
  - Calling agent ID participant check
  - Turn sequence verification (`turn_number == currentTurn + 1`)
  - Document contract validation
  - Body SHA-256 normalized hash verification on even turns

### 7.3 `NcursesConsole` (`include/NcursesConsole.hxx`, `src/NcursesConsole.cxx`)
- Add CLI commands:
  - `collab <plan_file> [initiator] [reviewer]`
  - `collab status`
  - `collab nudge`
  - `collab takeover [agent_id]`
  - `collab abort`
- Update header status bar to display live collaboration widget:
  `[Collab: plan_new_feature.md | Turn 2: Reviewer (Cursor) | IN_PROGRESS]`

### 7.4 `WebServer` (`src/WebServer.cxx`) & Web Dashboard
- Add endpoints:
  - `POST /api/collaboration/start`
  - `GET /api/collaboration/status`
  - `POST /api/collaboration/collaborate`
  - `POST /api/collaboration/nudge`
  - `POST /api/collaboration/takeover`
  - `POST /api/collaboration/abort`
- Add collaboration card to Web Dashboard with live turn counter, participant badges, and side-channel text.

---

## 8. Implementation Plan & Staged Verification

### Stage 1: Bus & Daemon Logic
- Implement `CollaborationSession`, CRLF-normalizing hash checker, document contract parser, and single-session latching in `AgentMessageBus`.
- Add unit-level validations in `src/AgentMessageBus.cxx`.
- Compile via `make -j$(nproc)`.

### Stage 2: MCP Tool Wiring & Server Endpoints
- Implement `agent_collaborate` and `agent_wait_turn` in `McpServer.cxx`.
- Add REST endpoints in `WebServer.cxx`.
- Add console commands (`collab`, `status`, `nudge`, `takeover`, `abort`) in `NcursesConsole.cxx`.
- Compile via `make -j$(nproc)`.

### Stage 3: Automated Test Verification
Write scratch test script verifying:
1. **Kickoff Latch**: Kickoff latches Turn 1 for `agent-antigravity-builder`. Late arrivals calling `agent_wait_turn` immediately receive `turn_number: 1`.
2. **Document Contract & Hash Storing**: Initiator Turn 1 completion validates document contract, sets `planBodyHash`, advances to Turn 2, and wakes reviewer.
3. **CRLF Invariance**: A Windows CRLF formatted body with identical text matches `planBodyHash`.
4. **Body Tampering Rejection**: Reviewer attempting to alter plan body above `## Collaboration` is **rejected** with `COLLABORATION_ERROR: Plan body SHA-256 mismatch`.
5. **Reviewer Critique**: Compliant Reviewer Turn 2 critique succeeds, advances to Turn 3, and wakes initiator.
6. **Initiator Re-work**: Initiator Turn 3 re-works plan body, addresses feedback, advances to Turn 4, and wakes reviewer.
7. **Terminal Consensus**: Reviewer Turn 4 signals `CONSENSUS_REACHED` $\rightarrow$ transitions session to `CONSENSUS_REACHED`, `nextActorId = NONE`, wakes all waiters with `consensus_reached`, and does **not** schedule Turn 5.
8. **Short-Poll Idleness**: Short-poll with `timeout_seconds > 0` returns `status: idle` without clearing or advancing the latch.
9. **Turn 8 Cap Escalation**: Simulated unagreed Turn 8 transitions session to `OPERATOR_REVIEW` and ceases peer wakeup.

---

## 9. C++ Coding Style & Standards Compliance

- BSD style 4-space indentation (`indent-tabs-mode: nil`).
- File naming: `.hxx` and `.cxx` in PascalCase.
- Standard headers: `Copyright (C) 2026, Charles Chiou`.
- Emacs modeline footers on all source files.
- Zero corporate or third-party entity names.
- Native compilation strictly via top-level `Makefile` (`make -j$(nproc)`).

---

## License & Copyright

Copyright (C) 2026, Charles Chiou. All rights reserved.
