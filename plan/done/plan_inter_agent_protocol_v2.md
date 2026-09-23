# Inter-Agent Conversation Protocol Specification (v2)

**Status**: CONSENSUS_FROZEN (Formally Ratified & Signed Off by Antigravity & Cursor Grok)  
**Task ID**: `task-protocol-v2`  
**State Hash (SHA-256)**: `935cfe77d18dc7343a314421862827f02773e40dbe12eef16b94c5e6b5f5cd75`  
**Consensus Timestamp**: 2026-09-16T21:14:00+08:00  
**Participants & Sign-off**:
- `agent-antigravity-builder` (Google Antigravity / Gemini) — `VOTE: ACK` (2026-09-16T21:14:00+08:00)
- `agent-cursor-windows` (Cursor Grok / Windows) — `VOTE: ACK` (2026-09-16T21:13:14+08:00, ref: `msg-108`)

---

## 1. Architectural Motivation & Scope

Protocol v1 established basic human-in-the-loop inter-agent messaging with initial operator authorization and real-time dual-screen monitoring. However, production multi-agent collaboration exposed three critical failure modes:
1. **Ambiguous String Identities**: Display names (`Cursor Grok`, `Cursor (Windows)`) caused silent matching failures and routing ambiguity.
2. **Destructive Queue Inboxes**: `agent_check_inbox` acted as a destructive queue pop rather than an append-only log cursor, causing message loss during reconnects and multi-agent polling.
3. **Underspecified Consensus Semantics**: Lack of formal NACK handling, write leases, and plan hashing created races and risk of divergent state.

Protocol v2 resolves these issues with a strict header envelope, non-destructive replay log, clear turn-taking authorization, and a two-phase consensus protocol.

---

## 2. Pillar 1: Canonical Identity & Structured Message Envelope

### 2.1 Canonical Agent Identity
- Display names (e.g. `"Cursor Grok"`, `"Antigravity IDE"`) are treated purely as UI metadata.
- All protocol-level routing, authorization, and turn handovers bind exclusively to canonical agent IDs:
  - `sender_id`: Canonical identifier of the emitting agent (e.g. `agent-cursor-windows`).
  - `target_id`: Canonical identifier of the intended recipient (e.g. `agent-antigravity-builder`).
  - `next_actor_id`: Canonical identifier authorized to post the next message.
- **Session Rebind**: `register_agent_task` rebinds any rotating transient transport session (e.g. SSE `session_id`) onto the canonical `agent_id`. All bus routing queries strictly use `agent_id`.

### 2.2 Monotonic Sequence & Envelope Schema
All dialogue records belong to an append-only sequence numbered by `seq` (monotonic `uint64` per conversation). The message envelope is split into a **strictly validated header** and an **extensible typed payload**:

```json
{
  "protocol_version": 2,
  "conversation_id": "convo-1",
  "seq": 107,
  "message_id": "msg-107",
  "correlation_id": "msg-106",
  "sender_id": "agent-antigravity-builder",
  "target_id": "agent-cursor-windows",
  "type": "AMENDMENT",
  "next_actor_id": "agent-cursor-windows",
  "turn_number": 3,
  "task_id": "task-protocol-v2",
  "timestamp_epoch": 1789564300,
  "expected_action": "CRITIQUE | AMENDMENT | VOTE | ACKNOWLEDGE",
  "payload": {
    "section_id": "all",
    "summary": "Turn 3 amendment addressing all Turn 2 critique points",
    "content": "...",
    "state_hash": "935cfe77d18dc7343a314421862827f02773e40dbe12eef16b94c5e6b5f5cd75",
    "byte_len": 10060,
    "ballot": null,
    "blocker_ids": []
  }
}
```

### 2.3 Enumerated Message Types
- `PROPOSAL`: Introduction of a new specification section or design choice.
- `CRITIQUE`: Itemized objections, failure modes, or edge cases against a prior proposal.
- `AMENDMENT`: Formal revision addressing critique points.
- `VOTE_REQUEST`: Call to freeze a specific `section_id` or the whole document. Requires `state_hash` and `byte_len`. (Note: when `section_id=all` and the latest `AMENDMENT` already carries matching `state_hash` + `byte_len`, the peer may issue `VOTE: ACK` directly).
- `VOTE`: Formal vote. Payload must specify `ballot`: `"ACK"` or `"NACK"`. If `NACK`, payload must include non-empty `blocker_ids[]`.
- `HALT`: Mutual confirmation of consensus and formal session close. `next_actor_id` must be `"NONE"`.
- `PROTOCOL_ERROR`: Emitted when an envelope fails header validation, out-of-turn post occurs, or version skew is detected. Does not advance `turn_number` or consume the turn quota.

### 2.4 Header Validation Rules
- The `aimon` daemon strictly validates header presence, types, and sequence monotonicity.
- Envelopes with missing required headers or invalid enums trigger `PROTOCOL_ERROR` and alert the operator console without waking the peer agent.
- `expected_action` must be one of `{CRITIQUE, AMENDMENT, VOTE, ACKNOWLEDGE}` and must be consistent with `type` (e.g. `VOTE_REQUEST` $\rightarrow$ `VOTE`).

---

## 3. Pillar 2: Turn-Taking, Anti-Deadlock & The Three Clocks

### 3.1 Strict Next-Actor Authorization
- `aimon` rejects message posts from any agent other than the authorized `next_actor_id`.
- Exceptions: Human operator injection or an explicit `HALT` broadcast.
- Prevents dual-agent write races where both peers respond simultaneously.

### 3.2 Bounded Turn Limits & Circuit Breaker
- Default conversation quota: **`max_turns = 16`**.
- Operator alert triggered at `turn_number = 12`.
- `PROTOCOL_ERROR`, keep-alive pings, and heartbeat records are excluded from `turn_number` consumption.
- If `turn_number` reaches 16 without consensus, `aimon` enters `STALLED` state and prompts the operator for manual resolution (`continue`, `break`, or `terminate`).

### 3.3 The Three Clocks (SLA & Timeout Decoupling)
To eliminate false stalls with remote Windows SSE/MCP clients, timeouts are partitioned into three independent clocks:

1. **Transport Reconnect Clock (15–30s)**:
   - Polling / HTTP connection drops are silent.
   - Client reconnects seamlessly using `Last-Event-ID` or `since_seq` without raising operator alarms.
2. **Turn SLA Clock (Agent Think Time)**:
   - **180s**: Non-blocking status notice in operator console (`Agent thinking...`).
   - **300s**: Daemon sends an in-channel keep-alive nudge. Nudge does not advance `turn_number` and does not alter `next_actor_id`.
   - **600s**: Daemon alerts operator with option for `TAKEOVER` or manual prompt nudge.
3. **Inactivity Session Expiry Clock (1800s)**:
   - If an active conversation experiences zero activity for 30 minutes, `aimon` safely transitions state to `TERMINATED_IDLE` to release memory and socket handles.

---

## 4. Pillar 3: Non-Destructive Replay Log & Artifact Binding

### 4.1 SQLite Dialogue Replay Log (Source of Truth)
- All dialogue records are appended to `aimon.db` table `agent_dialogue_records`:
  ```sql
  CREATE TABLE IF NOT EXISTS agent_dialogue_records (
      conversation_id TEXT NOT NULL,
      seq INTEGER NOT NULL,
      message_id TEXT NOT NULL PRIMARY KEY,
      correlation_id TEXT,
      sender_id TEXT NOT NULL,
      target_id TEXT NOT NULL,
      type TEXT NOT NULL,
      envelope_json TEXT NOT NULL,
      created_at INTEGER NOT NULL
  );
  CREATE UNIQUE INDEX IF NOT EXISTS idx_dialogue_convo_seq ON agent_dialogue_records(conversation_id, seq);
  CREATE INDEX IF NOT EXISTS idx_dialogue_target ON agent_dialogue_records(conversation_id, target_id, seq);
  ```
- **SSE Stream**: Server emits events formatted as:
  ```text
  id: convo-1:107
  event: agent_message
  data: <envelope_json>
  ```
- Reconnection uses W3C standard `Last-Event-ID: convo-1:106` to replay all messages with `seq > 106`.
- **MCP `agent_check_inbox`**: Takes `since_seq` parameter and filters strictly by `target_id`. It operates as a non-destructive cursor over the database, completely preventing message loss or cross-agent inbox poisoning.

### 4.2 Task & Plan Binding with Single-Writer Lease
- Every conversation binds 1-to-1 with an active `task_id` in `TaskRegistry` and an authoritative plan file (`plan/plan_<name>.md`).
- **Single-Writer Lease**: Only the agent holding `next_actor_id` during its active turn holds the write lease to modify the plan file on disk. The peer agent holds read-only lease. The operator may break or reassign the lease at any time.
- Bus envelopes carry `section_id` diffs/amendments and canonical SHA-256 `state_hash` of the target plan file, eliminating the overhead of transmitting full multi-kilobyte documents on every turn.

---

## 5. Pillar 4: Two-Phase Consensus Handshake

### 5.1 Staged Agreement Lifecycle
1. **Per-Section Consensus**: Individual sections (e.g. `Pillar 1`, `Pillar 2`) can be voted on and marked `FROZEN` iteratively during negotiation.
2. **Phase 1: Whole-Document Freeze Request (`VOTE_REQUEST`)**:
   - Proposing agent verifies all sections are resolved.
   - Computes SHA-256 checksum and UTF-8 byte length of `plan/plan_<name>.md`.
   - Dispatches `VOTE_REQUEST` with `payload: { state_hash: "<hash>", byte_len: <len> }`.
3. **Phase 2: Formal Vote (`VOTE`)**:
   - Peer agent reads local plan file, computes independent SHA-256 hash, and verifies against project guidelines:
     - BSD C++ coding style (no tabs, 4 spaces, `.cxx`/`.hxx`).
     - Strict personal copyright attribution: `Copyright (C) 2026, Charles Chiou`.
     - Zero third-party corporate or employer entity names.
     - Process & screen deployment topology.
   - If compliant: emits `VOTE: ACK` containing `{ agent_id, state_hash, timestamp }`.
   - If non-compliant: emits `VOTE: NACK` containing explicit `blocker_ids[]`.
4. **NACK Escalation Guard**:
   - Upon `NACK`, `next_actor_id` returns to proposer with `expected_action: AMENDMENT`.
   - If two consecutive `NACK` votes occur on the identical `blocker_id` without amendment, `aimon` halts turn-taking and requests operator intervention.
5. **Consensus Seal & Session Close (`HALT`)**:
   - When both agents have registered `VOTE: ACK` on matching `state_hash`, the plan status is updated to `CONSENSUS_FROZEN`.
   - A final `HALT` envelope with `next_actor_id: "NONE"` is emitted.
   - Task `task_id` in `TaskRegistry` transitions to `completed`.

---

## 6. Operator Governance & Oversight

- The human operator remains authoritative at all times via `aimon` CLI and Web Dashboard:
  - `approve <id>`: Authorizes session kickoff.
  - `reject <id>`: Denies session request.
  - `terminate <id>`: Immediately terminates an active conversation.
  - `takeover <id>`: Pauses inter-agent turns and assigns write lease directly to operator.
  - `inject <id> "<message>"`: Injects an operator instruction; explicitly designates `next_actor_id`.

---

## 7. Ratification & Consensus Record

This specification is ratified and frozen by mutual sign-off:
- **`agent-cursor-windows`**: Verified and voted `ACK` via message `msg-108`.
- **`agent-antigravity-builder`**: Verified and voted `ACK` via message `msg-109`.
- **Target Implementation**: To be integrated into `AgentMessageBus`, `McpServer`, `WebServer`, and `TaskRegistry` in `aimon v1.1.0`.

---

## License & Copyright

Copyright (C) 2026, Charles Chiou. All rights reserved.
