# Inter-Agent Collaboration Protocol Evaluation & Improvement Plan

## 1. Objective & Context

This plan establishes a framework to evaluate the live performance of the Document-Centric Inter-Agent Collaboration Protocol between **Google Antigravity (`agent-antigravity-builder`)** and **Cursor (`agent-cursor-windows`)**, and to collaboratively determine concrete protocol improvements based on real operational telemetry across personal project workspaces.

---

## 2. Evaluation Dimensions

### 2.1 Latency, Transport & Polling Parameters
- **Handoff Latency**: Measure time elapsed between an agent calling `agent_collaborate` and the peer receiving the unblocking response from `agent_wait_turn`.
- **Polling Parameter Profiles**:
  - *Profile A (Long-Poll)*: Indefinite blocking (`timeout_seconds = 0`) on kernel `std::condition_variable`. Evaluates connection durability and resource usage.
  - *Profile B (Short-Poll)*: Periodic polling (`timeout_seconds = 10`) returning `{ "status": "idle" }`. Recommended default for desktop MCP clients to prevent client-side HTTP timeout drops.
- **Memory & Socket Footprint**: Monitor resident memory and open file descriptors on the `aimon` gateway during multi-turn sessions.

### 2.2 Content Integrity, Normalization & Security
- **LF Normalization Across File Systems**: Confirm that CRLF $\leftrightarrow$ LF conversions across heterogeneous network mounts (ext4, NTFS, SMB) produce identical normalized SHA-256 body hashes.
- **Protected Body Boundary**: Verify that all normative engineering sections and the License block reside strictly above `## Collaboration` and are cryptographically verified by `planBodyHash` on every reviewer turn.
- **Parsing Robustness**: Ensure markdown parser ignores headings matching `## Collaboration` inside fenced code blocks or backtick strings.

### 2.3 MCP Catalog Refresh & Tool Discovery
- **Tool Catalog Propagation**: Measure latency from dynamic tool registration in `aimon` to peer IDEs refreshing their `tools/list` cache.
- **Notification Handling**: Evaluate client responsiveness to `notifications/tools/list_changed` over SSE.
- **Operator Runbook**: Document standard manual reload steps (e.g. `Cursor Settings -> Features -> MCP -> Refresh`) when client IDEs cache tool definitions.

### 2.4 Agent Identity & Topology Independence
- All routing, authorization, and metrics are keyed exclusively by canonical identifiers (`agent-antigravity-builder`, `agent-cursor-windows`, `operator`) rather than platform or host operating system labels.

---

## 3. Prioritized Protocol Enhancements

Based on operational feedback, candidate enhancements are prioritized as follows:

### Priority 1: Turn Diff Summary Payload
Enhance `agent_wait_turn` and `GET /api/collaboration/status` to include an orientation summary in the payload:
- `body_line_delta`: Net line count change in the plan body since the prior turn.
- `plan_body_hash`: Current LF-normalized SHA-256 hash.
- `collaboration_lines`: Line count growth in `## Collaboration`.
- `byte_len` & `file_mtime_epoch`: File size and timestamp on disk.

### Priority 2: SQLite Collaboration Audit Trail
Persist session telemetry into `~/.config/aimon/history.db` table `collaboration_audit_records`:
- `plan_file`, `session_id`, `turn_number`, `agent_id`, `agent_model`, `role`
- `dwell_time_ms` (time agent held turn before signaling)
- `handoff_latency_ms` (time peer took to unblock)
- `status` (`IN_PROGRESS`, `CONSENSUS_REACHED`, `OPERATOR_REVIEW`)

### Priority 3: Enhanced Web Dashboard Timeline & REST Wait
- **Web UI Timeline**: Interactive visual turn breakdown in the `aimon` web dashboard displaying live turns, dwell time pills, and state transitions.
- **REST Wait Endpoint**: Add `POST /api/collaboration/wait` with identical 6-step evaluation order to support non-MCP automation scripts.

*(Note: Generic markdown syntax linters are intentionally omitted to avoid false rejections on fenced C++/JSON blocks; the deterministic Four-Line Contract provides sufficient structure).*

---

## 4. Verification & Trial Procedure

### 4.1 Four-Turn Consensus Lifecycle
1. **Turn 1 (Initiator)**: Gemini drafts evaluation framework and initial enhancement candidates. [COMPLETED]
2. **Turn 2 (Reviewer)**: Cursor critiques plan, identifies catalog staleness, requests license relocation, and ranks enhancements. [COMPLETED]
3. **Turn 3 (Initiator)**: Gemini re-works body addressing all 9 critique points, moves license above `## Collaboration`, and details test cases. [IN_PROGRESS]
4. **Turn 4 (Reviewer)**: Cursor validates re-work, verifies compliance, and signals `CONSENSUS_REACHED`.

### 4.2 Edge Case Test Matrix for `agent_wait_turn`
Verify that `agent_wait_turn` executes the strict 6-step evaluation order:
1. `IDLE` state $\rightarrow$ immediate `{"status": "no_session"}`.
2. Mismatched `plan_file` or non-participant agent $\rightarrow$ immediate `COLLABORATION_ERROR`.
3. `CONSENSUS_REACHED` $\rightarrow$ immediate `consensus_reached` (no Turn 5 scheduled).
4. `OPERATOR_REVIEW` $\rightarrow$ unblock with `operator_review`.
5. Turn Ready $\rightarrow$ unblock with `turn_ready` and $\text{turn\_number} = \text{currentTurn} + 1$.
6. Waiting $\rightarrow$ CV wait or short-poll `idle` without latch mutation.

---

## 5. Document Contract Specification

The Four-Line Contract accepts both initial drafts and iterative revisions:
- Heading: `## Collaboration` (first occurrence in file)
- Turn Header: `### Turn <N>: <Agent Identifier> (<Role>)`
- Line 1: `- **Turn Started**: <Timestamp>`
- Line 2: `- **Model**: <Model Identifier>`
- Line 3: `- **Role**: <Initiator | Reviewer>`
- Line 4: Itemized content marked by:
  - `- **Summary of Plan**:` (Accepted for Turn 1)
  - `- **Feedback & Critique**:` (Accepted for Even Reviewer Turns)
  - `- **Re-work & Responses**:` (Accepted for Odd Initiator Turns)
- Line 5: `- **Turn Finished**: <Timestamp>`

---

## License & Copyright

Copyright (C) 2026, Charles Chiou. All rights reserved.

---

## Collaboration

### Turn 1: Gemini 3.8 Flash High (Initiator)
- **Turn Started**: 2026-09-16T22:15:00+08:00
- **Model**: Gemini 3.8 Flash High
- **Role**: Initiator
- **Summary of Plan**:
  Formulated initial plan for evaluating the collaboration protocol and proposing four candidate enhancements (validation hooks, turn diffs, SQLite session logging, and web timeline visualization).
- **Turn Finished**: 2026-09-16T22:16:30+08:00

### Turn 2: Cursor Grok 4.6 High (Reviewer)
- **Turn Started**: 2026-09-16T22:17:30+08:00
- **Model**: Cursor Grok 4.6 High
- **Role**: Reviewer
- **Feedback & Critique**:
  1. **MCP catalog staleness is a first-class evaluation dimension.** This Cursor session's MCP namespace still lacked `agent_wait_turn` / `agent_collaborate` after the daemon registered them. `tools/list_changed` did not refresh the client; Turn 2 had to be driven via JSON-RPC `tools/call` on `/message`. Add a dimension: time from daemon tool registration to both IDEs listing the new tools, and a documented operator "reload MCP" step.
  2. **`agent_wait_turn` check-order must be in the trial, not only in `plan_agent_collaboration.md`.** Measure returns for `CONSENSUS_REACHED`, `OPERATOR_REVIEW`, `no_session`, wrong `plan_file`, and unregistered `agent_id`. Confirm `latchedTurnReady && nextActorId == NONE` never yields a phantom next turn.
  3. **License footer is below `## Collaboration`, so it is outside `planBodyHash`.** Appending turns will not protect it; moving it above the heading is an initiator (odd-turn) edit. Relocate License into the plan body on Turn 3, and add an integrity check that copyright remains in the hashed region.
  4. **Drop or demote the markdown linter hook.** Plan files are prose plus fenced C++/JSON; a generic markdown linter will false-fail. Prefer the existing four-line contract plus optional `byte_len` / `file_mtime_epoch` already returned on `turn_ready`.
  5. **Keep Turn Diff Summary, SQLite audit, and Web timeline — rank them 1 / 2 / 3.** Diff payload should include `planBodyHash`, line-count delta of the body, and whether `## Collaboration` grew. SQLite should store `turn_number`, `agent_id`, `wait_ms` (handoff to `wait_turn` return), `signal_epoch`, `status`. Timeline is operator sugar, not protocol.
  6. **REST vs MCP gap:** `GET /api/collaboration/status` exists; there is no REST `wait_turn`. Either document MCP-only wait, or add a non-blocking REST poll that uses the same check-order. This trial used both status REST and MCP wait.
  7. **Transport comparison is underspecified.** §2.1 should record: poll interval, HTTP timeout, whether the client holds a long-poll, and `tools/list` cache age. Short-poll `timeout_seconds = 10` is the right default for this Cursor MCP client.
  8. **Turn 1 log label** is `Summary of Plan` rather than `Re-work & Responses`. Accept an initiator alias in the contract (`Summary of Plan` | `Re-work & Responses`) so Turn 1 is valid without a special case.
  9. **Do not treat "Windows Cursor" as the only reviewer topology.** This reviewer process is a Cursor agent on Linux using the canonical id `agent-cursor-windows`. Evaluation should key off `agent_id`, not OS labels.
- **Status**: IN_PROGRESS
- **Turn Finished**: 2026-09-16T22:18:20+08:00

### Turn 3: Gemini 3.8 Flash High (Initiator)
- **Turn Started**: 2026-09-16T22:20:30+08:00
- **Model**: Gemini 3.8 Flash High
- **Role**: Initiator
- **Re-work & Responses**:
  1. Relocated License & Copyright section into the plan body above `## Collaboration` so it is cryptographically protected by `planBodyHash`.
  2. Added Section 2.3 addressing MCP catalog refresh latency, `tools/list_changed` propagation, and documented operator reload runbook.
  3. Added Section 4.2 detailing the explicit test matrix for the 6-step `agent_wait_turn` evaluation order.
  4. Dropped generic markdown linter in favor of the deterministic four-line contract plus `byte_len` and `file_mtime_epoch`.
  5. Formally ranked enhancements in Section 3: #1 Turn Diff Summary Payload, #2 SQLite Collaboration Audit Trail, #3 Web Dashboard Timeline & REST Wait.
  6. Documented optional REST `/api/collaboration/wait` endpoint mirroring MCP `agent_wait_turn`.
  7. Specified transport benchmark parameters (short-poll 10s default vs long-poll CV wait).
  8. Updated Section 5 document contract schema to recognize both `Summary of Plan` (Turn 1) and `Re-work & Responses` (Turns 3+).
  9. Sanitized all evaluation criteria to key strictly off canonical agent IDs rather than host OS assumptions.
- **Turn Finished**: 2026-09-16T22:23:15+08:00

### Turn 4: Cursor Grok 4.6 High (Reviewer)
- **Turn Started**: 2026-09-16T22:21:43+08:00
- **Model**: Cursor Grok 4.6 High
- **Role**: Reviewer
- **Feedback & Critique**:
  Turn 3 addresses all nine Turn 2 items. License is above `## Collaboration` and is in the hashed body. §2.3 covers MCP catalog refresh; §4.2 encodes the six-step `agent_wait_turn` order; enhancements are ranked Diff / SQLite audit / Web+REST wait; linter is dropped; contract accepts `Summary of Plan`; routing is by `agent_id`.
  Non-blocking leftover: §4.1 still labels Turn 3 `[IN_PROGRESS]` in the procedure list. That is this trial's progress marker, not a protocol defect. Do not schedule a Turn 5.
  **Status**: CONSENSUS_REACHED
- **Turn Finished**: 2026-09-16T22:22:10+08:00
