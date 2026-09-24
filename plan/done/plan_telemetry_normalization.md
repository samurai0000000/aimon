# Telemetry Database Normalization & Activity Timeline Realignment Plan

- **Status**: `Done`
- **Lifecycle Location**: `plan/done/`

## 1. Motivation & Problem Analysis

Recent inspection of `~/.config/aimon/telemetry.db` and the live telemetry dashboard (`http://builder.selfso.com:3883/#telemetry`) identified four major data distortion issues:

1. **5+ Million Duplicate Event Rows in Database**:
   `AntigravityCollector::syncTranscriptTelemetry()` maintained file offsets in an in-memory `std::map`. On every daemon restart, it re-read all historical transcript JSONL files from byte 0 and performed blind `INSERT INTO agent_lifecycle_events` queries. This inflated database rows to over 5,050,000 and distorted tool velocity charts to a false 5.6 calls/sec.
2. **Approval Latches Registered as "Agent Sessions"**:
   Mobile approval requests emitted events under `session_id = "approval-<uuid>"`. The session auto-upsert logic treated every individual approval popup as an independent agent session, inflating peak concurrency to 14 simultaneous agents and filling the Gantt chart with 0-second approval slivers.
3. **Session Duration Distortions (3 to 31-Hour Continuous Blocks)**:
   Antigravity transcript conversations set `start_timestamp = first_event` and `end_timestamp = last_event`. Conversations spanning multiple days were graphed as 100% continuously executing agents across every 1-hour interval, rather than discrete, turn-based bursts.
4. **Transient Test Sessions in UI Dropdowns**:
   Test keys (`hook-selftest`, `t1`, `default_session`, `s1`, `test-cursor-1`, `default`) were listed in the main session dropdown.

---

## 2. Frozen Decisions & Architectural Strategy

- **Complete Database Purge & Deduplication**:
  - Purge all duplicate rows from `agent_lifecycle_events`, keeping only unique records per `(session_id, step_index, event_type, tool_name)`.
  - Delete all `approval-%` pseudo-sessions from `agent_sessions`.
  - Delete transient test sessions from `agent_sessions`.
  - Run `VACUUM;` to reclaim disk space (shrinking `telemetry.db` from ~750MB to ~5–10MB).
- **Idempotent Ingest Pipeline**:
  - Enforce a `UNIQUE INDEX` on `agent_lifecycle_events(session_id, step_index, event_type, tool_name)`.
  - Use `INSERT OR IGNORE INTO agent_lifecycle_events` across all collectors and HTTP ingestion endpoints.
- **Approval Session Isolation**:
  - Approval events (`session_id LIKE 'approval-%'`) remain recorded in `agent_lifecycle_events` for security audit history, but are **strictly excluded from `agent_sessions`**.
- **Burst-Based Concurrency Modeling**:
  - Compute concurrency in `queryActivityTimeline()` by aggregating actual event presence in each time bucket, rather than assuming uninterrupted continuous execution across days.

---

## 3. Staged Implementation Envelopes

### Envelope 1: Database Purge & Schema Hardening
- **Target Files**: [`src/AgentTelemetryDb.cxx`](../../src/AgentTelemetryDb.cxx), [`include/AgentTelemetryDb.hxx`](../../include/AgentTelemetryDb.hxx)
- **Actions**:
  1. Add unique constraint:
     ```sql
     CREATE UNIQUE INDEX IF NOT EXISTS idx_events_unique 
     ON agent_lifecycle_events(session_id, step_index, event_type, tool_name);
     ```
  2. Implement database cleanup in `AgentTelemetryDb::open()` / `initSchema()`:
     ```sql
     -- 1. Remove duplicate lifecycle events
     DELETE FROM agent_lifecycle_events 
     WHERE id NOT IN (
         SELECT MIN(id) 
         FROM agent_lifecycle_events 
         GROUP BY session_id, step_index, event_type, tool_name
     );

     -- 2. Remove approval pseudo-sessions
     DELETE FROM agent_sessions WHERE session_id LIKE 'approval-%';

     -- 3. Remove transient test sessions
     DELETE FROM agent_sessions 
     WHERE session_id IN ('hook-selftest', 't1', 's1', 'default_session', 'test-cursor-1', 'default');
     ```
  3. Update `_stmtInsertEvent` to `INSERT OR IGNORE INTO agent_lifecycle_events ...`.

### Envelope 2: Ingestion & Collector Hardening
- **Target Files**: [`src/AgentTelemetryDb.cxx`](../../src/AgentTelemetryDb.cxx), [`src/AntigravityCollector.cxx`](../../src/AntigravityCollector.cxx)
- **Actions**:
  1. In `insertEvent()` & `insertEventsBatch()`:
     - Check `if (event.sessionId.rfind("approval-", 0) == 0) return ok;` before triggering `_stmtUpsertSession`.
     - Filter out test session IDs (`hook-selftest`, `t1`, `s1`, `test-cursor-1`).
  2. In `AntigravityCollector.cxx`:
     - Ensure batch inserts use `insertEventsBatch()` with `INSERT OR IGNORE`.

### Envelope 3: Activity Timeline & Concurrency Realignment
- **Target Files**: [`src/AgentTelemetryDb.cxx`](../../src/AgentTelemetryDb.cxx), [`web/app.js`](../../web/app.js)
- **Actions**:
  1. In `queryActivityTimeline()`:
     - Query `agent_sessions` filtering out `session_id NOT LIKE 'approval-%'` and valid tools/turns.
     - Compute `antigravity_active` and `cursor_active` per time bucket based on actual event activity within the bucket window, capping inactive intervals.
  2. In `querySessions()`:
     - Exclude `approval-%` and transient test session IDs.
     - Order by `start_timestamp DESC`.

---

## 4. Verification & Testing Plan

1. **Unit Test Suite**:
   - Run `./build/test_agent_telemetry_db`, `./build/test_mobile_gateway`, `./build/test_web_server_api`.
2. **Database Sanity Check**:
   - Verify row count in `agent_lifecycle_events` is reduced to canonical events (~50k vs 5.05M).
   - Verify `agent_sessions` contains only legitimate development sessions (e.g. `3b7a2499...`, `sess-30e9502b...`).
3. **Live Dashboard Inspection**:
   - Capture headless Chromium screenshot of `http://127.0.0.1:3883/#telemetry`.
   - Verify:
     - Tool velocity reflects actual single-digit / fractional call rates.
     - Concurrency shows 1 active agent.
     - Gantt chart displays legitimate Antigravity & Cursor development sessions.
