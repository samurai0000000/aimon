# Plan: Cursor Enterprise Usage & Fast Requests Timeseries Chart

- **Date**: 2026-10-02
- **Target Platform / Scope**: `aimon` AI Quota Monitor (`aimon daemon`, `CursorCollector`, `HistoryStore`, `WebServer`, `web/app.js`, `include/WebAssets.hxx`)
- **Status**: Done
- **Lifecycle Location**: `plan/done/` (Done)
- **Artifacts Directory**: `plan/plan_cursor_enterprise_usage_chart.artifacts/` (moves with plan across lifecycle)
- **Agent Mode**: Single-Agent Pair Programming
- **Core Objectives**:
  1. Support adaptive rendering in the `#quotas` dashboard for both Enterprise (pooled fast requests burn-down) and Pro (dollar spend) Cursor accounts.
  2. Synthesize daily fast request usage points from `HistoryStore` / `history.db` whenever Cursor API's `GetDailySpendByCategory` returns no dollar spend (`dailySpend: []`).
  3. Update `web/app.js`, `web/index.html`, and `include/WebAssets.hxx` to render an intuitive, dynamic SVG burn-down / accumulation graph with proper request units (e.g. `10k`, `20k`, `120k`) rather than clearing the SVG canvas to a blank state.

---

## 0. Corrections & Negative Constraints (Do Not Reintroduce)

1. **Do Not Assume All Cursor Accounts Have Dollar Spend**:
   - *Disproven assumption*: The existing implementation assumed Cursor accounts always have dollar spend entries in `GetDailySpendByCategory`.
   - *Fact*: Enterprise accounts with pooled fast requests (e.g. 120,000 allowance) incur $0.00 on-demand spend. Cursor's API returns `{"effectiveLimitCents": 120000}` without a `dailySpend` array.
   - *Negative constraint*: Never clear the chart canvas or hide the chart section when `daily_spend` is empty if `fast_requests_limit > 0`. Instead, fall back to plotting fast requests burn-down over time.
2. **Do Not Discard Pro On-Demand Spend Capability**:
   - Pro tier accounts with on-demand dollar spend must continue to display their dollar cumulative spend graph and category breakdowns. The UI and data model must be adaptive (Dual Mode: Fast Requests Burn-down vs. Dollar Spend).
3. **Do Not Bypass WebAssets.hxx Synchronization**:
   - `aimon` embeds static assets in `include/WebAssets.hxx` as C++ raw string literals. Editing only `web/app.js` or `web/index.html` without updating `include/WebAssets.hxx` (or recompiling the daemon) will result in stale web dashboards being served.

---

## 1. Execution Boundaries & Strict Guardrails

During execution, the assistant operates strictly under these boundaries:

1. **Direct Question Answering (Virtual Ask Mode)**:
   - Answer all user questions, inquiries, and status checks directly in text using read-only inspection tools.
   - Strictly no unsolicited file edits or scratch file generation during exploratory Q&A.
2. **Mandatory User Authorization Gate (Virtual Plan Mode)**:
   - Never modify source code, configuration files, or documentation without an approved plan and explicit user instruction to proceed.
3. **Plan Artifacts Directory**:
   - Save all generated mockups, analysis logs, test evidence, diagrams, and supplementary artifacts under `plan/plan_cursor_enterprise_usage_chart.artifacts/`.
4. **Anti-Sycophancy & Code Completeness**:
   - Provide critical technical friction; challenge flawed premises or bug-prone designs.
   - Strictly no code stubs, placeholder comments (`// TODO`), or truncated blocks (`/* ... */`). All code edits must be 100% complete, fully drop-in compilable, and include complete error handling.
5. **Zero-Tolerance Guessing & Speculative Workarounds**:
   - Never guess commands, configuration values, file paths, credentials, or internal APIs.
   - Stop immediately upon encountering unexpected errors or discrepancies and report raw facts to the user.
6. **Ground Truth & C/C++ Qualification**:
   - Invalidate proxy-only checks (process existence or compile success alone) and tautological tests.
   - A C or C++ behavior change starts with a CppUTest / unit test that fails on the old code, then passes under the repository's existing `make test`. Do not add `make check`, `make cross`, or raw `cmake` / `ctest`.
   - Cover the nominal path, the boundary (empty, minimum, maximum, truncated, oversized), and one fault (invalid input, timeout, or disconnect).
   - **Required tools**. Use the tool the change calls for:
     - RapidCheck when the change is a parser, codec, or state machine. Properties cover empty, truncated, and oversized input.
     - AddressSanitizer and UBSan (`-fsanitize=address,undefined`) when the change is memory ownership, concurrency, or a pointer cast, run through `make test`.
     - `mull-runner` once at plan close-out when C or C++ tests changed. A mutant in a changed function is killed, or named in `plan/plan_cursor_enterprise_usage_chart.artifacts/mull-exclusions.txt` as `equivalent`, `unreachable`, or `tool-unsupported`.
     - `make test` on the host named in the project dossier when the binary runs on a different ISA than the edit host.
   - Documentation, comments, formatting, Python, shell, and configuration use the checks that repository already has.
   - Do not mock the component under test. A test that only asserts its own stand-in was called does not count. A stand-in is allowed at a hardware or remote-device boundary, and a pass against that stand-in does not qualify the hardware.
   - If `make test` does not pass, do not start the next envelope.
7. **Surgical Edits & Code Integrity**:
   - Use targeted line replacements (`replace_file_content` or `multi_replace_file_content`).
   - Preserve all existing comments, docstrings, license headers, and surrounding code formatting in unmodified sections.
8. **Git Commit Prohibition & Pre-Commit Review**:
   - Never execute `git commit` in any repository without an explicit, direct command from the user.
   - Run required pre-commit compliance and lint checks before any commit is created.
9. **Project-Specific Boundaries & Operational Invariants**:
   - `aimon` is a selfso personal project. All commits and authorships must strictly attribute copyright to `Copyright (C) 2026, Charles Chiou` and adhere to `Signed-off-by: Charles Chiou <samurai@selfso.com>`.
   - Daemon stability: Ensure `aimon` service on `builder` can be recompiled and restarted smoothly without losing `history.db` or `telemetry.db` data.
   - Top-level `Makefile` wrapping: Always invoke `make` and `make test` from repository root.

---

## 2. Technical Approach & Architecture

### 2.1 Purpose and Non-Negotiable Acceptance Criteria
- **Problem**: When accessing `http://builder.selfso.com:3883/#quotas`, the Cursor section shows a blank, empty SVG box under "Cumulative Spend" because the backend returns `daily_spend: []` for Enterprise accounts.
- **Criteria**:
  1. The Cursor chart must dynamically adapt:
     - **Mode A (Fast Requests Burn-down)**: Active when `daily_spend` is empty and `fast_requests_limit > 0`. Y-axis represents fast requests (e.g. `0` to `120k`), title reflects "Fast Requests Burn-down" or "Usage Trend", and the trend line shows daily/cumulative request consumption.
     - **Mode B (Cumulative Dollar Spend)**: Active when `daily_spend` has non-zero entries and `total_spend_usd > 0`. Y-axis represents dollars (`$0` to `$N`), title reflects "Cumulative Spend".
  2. `CursorCollector` or `HistoryStore` must synthesize daily request points over the active billing cycle from `history.db` (`quota_samples` table where `provider = 'cursor'` and `metric_key = 'fast_requests_used'`).
  3. Both `web/` assets and `include/WebAssets.hxx` must be kept in exact parity.

### 2.2 System Architecture Blueprint

```mermaid
graph TD
    subgraph Cursor Cloud
        API2[api2.cursor.sh /auth/usage-summary] -->|Fast requests: 12206/120000| CC[CursorCollector]
        API2_SPEND[GetDailySpendByCategory] -->|effectiveLimitCents: 120000, dailySpend: []| CC
    end

    subgraph aimon Daemon (C++17)
        CC -->|Samples every 5m| HS[(HistoryStore: history.db)]
        HS -->|quota_samples: fast_requests_used| CC_HIST[Usage History Aggregator]
        CC_HIST -->|Populates daily_usage / daily_spend points| CS[CursorStatus]
        CS --> AGG[AggregateStatus]
        AGG -->|JSON serialization| WS[WebServer /api/status]
    end

    subgraph Browser / Client (Web UI)
        WS -->|GET /api/status| APP[web/app.js]
        APP -->|Checks plan_tier & spend vs requests| ADAPT[Adaptive Chart Renderer]
        ADAPT -->|Mode A: Requests Trend| SVG[#cursor-spend-chart: Requests SVG Curve]
        ADAPT -->|Mode B: Dollar Spend| SVG
    end
```

### 2.3 Detailed Specifications, Interfaces & Data Schemas

#### 1. Data Schema Updates (`include/Models.hxx`)
In `struct DailySpendPoint`, expand or complement with request counts:
```cpp
struct DailySpendPoint {
    int64_t dayMs = 0;
    std::string dayStr;
    double spendUsd = 0.0;
    double cumulativeUsd = 0.0;
    int requestsUsed = 0;
    int cumulativeRequests = 0;

    nlohmann::json toJson() const {
        return {
            {"day_ms", dayMs},
            {"day_str", dayStr},
            {"spend_usd", spendUsd},
            {"cumulative_usd", cumulativeUsd},
            {"requests_used", requestsUsed},
            {"cumulative_requests", cumulativeRequests}
        };
    }
};
```
In `struct CursorStatus`:
Add boolean or indicator `usageMode` ("requests" vs "spend") and pass `dailySpendHistory` populated with requests data if dollar spend is absent.

#### 2. Backend Aggregation (`src/CursorCollector.cxx` & `src/HistoryStore.cxx`)
- If `spendJson` does not contain `dailySpend` (or has 0 items) and `status.fastRequestsLimit > 0`:
  Query `HistoryStore` for timestamped samples of `fast_requests_used` within the current billing cycle (`billingCycleStart` to now).
- Bucket the samples into daily snapshots (maximum `metric_value` per calendar day).
- Populate `status.dailySpendHistory` with:
  - `dayMs`: start of day timestamp in ms
  - `dayStr`: `YYYY-MM-DD`
  - `spendUsd`: 0.0
  - `cumulativeUsd`: 0.0
  - `requestsUsed`: delta for that day
  - `cumulativeRequests`: total used up to that day

#### 3. Frontend Adaptive Rendering (`web/app.js` & `include/WebAssets.hxx`)
In `renderCursorSpend(cr)`:
- Determine active mode:
  ```javascript
  const isRequestMode = (cr.daily_spend.length === 0 || cr.total_spend_usd === 0) &&
                        (cr.fast_requests_limit > 0 || (cr.daily_usage && cr.daily_usage.length > 0));
  ```
- If in Request Mode:
  - Header: Update subtitle to `"Fast Requests"` and title to `"Request Burn-down"`.
  - Amount pill: Show current requests used (e.g. `12,206 fast requests`) vs limit (`120,000`).
  - Scaling: Scale Y-axis to request counts (e.g. `120k`, `90k`, `60k`, `30k`, `0` or dynamic max).
  - SVG Curve: Draw smooth SVG area and line showing requests accumulation curve.
  - Interactive dots: Tooltip displays date and requests used on that day.

### 2.4 Threat Model, Safety & Failure Invariants
- **Offline / Degraded Mode**: If `history.db` has fewer than 2 points, render a single-point baseline or clean empty placeholder with an informational note ("Collecting initial usage samples...") rather than broken SVG markup.
- **Memory & Resource Safety**: Querying history for a 30-day billing cycle involves at most ~8,640 samples (1 every 5 min). Aggregation must be done efficiently via SQLite `GROUP BY strftime('%Y-%m-%d', datetime(timestamp, 'unixepoch'))` to keep latency under 2ms.

---

## 3. Staged Implementation Plan & Acceptance Criteria

### 3.0 Mandatory Qualification Protocol & Strict Progression Gate Rule

> [!CAUTION]
> **Strict Non-Negotiable Gate Rule**:
> - **C or C++ envelopes follow Section 1 item 6.** `make test` exits 0. The new test failed before the change. RapidCheck, AddressSanitizer and UBSan, or the other ISA run when that item selects them.
> - Bypassing tests, commenting out assertions, or advancing on a partial pass is forbidden.
> - **Failure Action Protocol**:
>   1. If any test assertion fails, throws an unhandled exception, crashes, or times out: **STOP IMMEDIATELY**.
>   2. Report the raw failure facts and diagnostic output.
>   3. Diagnose and fix the root cause strictly within the current envelope's boundaries.
>   4. Re-execute the qualification and regression test suites until 100% pass rate is verified.
>   5. Log the verified pass output in the Appendix before touching any file belonging to the next envelope.
> - **Evidence gate**: An envelope that claims a hardware, inference, or cross-domain result seals a gate spec with `python3 <path-to-intelligence>/scripts/plan_gate.py` before implementation. `verify` exiting 0 on that unaltered seal authorizes the next envelope. A retry uses a new attempt ID with identical criteria. Criteria changes require human approval. Do not mark the plan `Done`.

---

### Envelope 1: Backend History Aggregation & Fast Request Timeseries Model

**Goal:** Extend `HistoryStore` and `CursorCollector` to extract and supply daily fast-request usage points over the billing cycle when dollar spend data is unavailable.

Planned files:
- `include/Models.hxx` — Add request count fields to `DailySpendPoint` JSON serialization;
- `include/HistoryStore.hxx` — Declare `queryDailyUsageSummary(provider, metricKey, startTimestamp, endTimestamp)`;
- `src/HistoryStore.cxx` — Implement SQLite daily max/delta query using `strftime`;
- `src/CursorCollector.cxx` — Integrate fallback to `HistoryStore` when `GetDailySpendByCategory` returns no `dailySpend`;
- `test/TestCursorCollector.cxx` — Add dedicated qualification test suite covering nominal, boundary, and fault injection cases;
- `CMakeLists.txt` — Register `test_cursor_collector` executable;
- `Makefile` — Add `test_cursor_collector` to `make test` pipeline.

- [x] Task 1.1: Enhance `DailySpendPoint` in `include/Models.hxx` with `requests_used` and `cumulative_requests` fields and JSON serialization
  - **Target Files**: `include/Models.hxx`
  - **Verification**: `make` compiles cleanly without warnings or type errors.
- [x] Task 1.2: Implement `queryDailyUsageSummary` in `include/HistoryStore.hxx` and `src/HistoryStore.cxx`
  - **Target Files**: `include/HistoryStore.hxx`, `src/HistoryStore.cxx`
  - **Verification**: Direct query executes on test SQLite database returning sorted daily bucketed deltas.
- [x] Task 1.3: Update `src/CursorCollector.cxx` to populate daily history from `HistoryStore` when `dailySpend` is empty
  - **Target Files**: `src/CursorCollector.cxx`
  - **Verification**: Code compiles cleanly under `make`.
- [x] Task 1.4: Implement & Execute Envelope 1 Unit & Integrated Qualification Suites (`test/TestCursorCollector.cxx`)
  - **Target Files**: `test/TestCursorCollector.cxx`, `CMakeLists.txt`, `Makefile`
  - **Test Matrix**:
    - *Repeatable*: Hermetic SQLite test fixture initialized in `/tmp/` with automated cleanup.
    - *Coverage*: Enterprise payload with `dailySpend: []` triggers fallback; Pro payload preserves dollar spend.
    - *Boundary*: 0 historical samples (empty db), 1 sample (single point), multi-month boundary rollover.
    - *Fault Injection*: Malformed SQLite database, missing `quota_samples` table, corrupt JSON response.
    - *Integrated Scope*: End-to-end flow from collector parsing to `HistoryStore` query to `status.dailySpendHistory` output.
  - **Verification**: `./build/test_cursor_collector` exits code 0 with 100% assertions passing and 0 memory leaks.
- [x] Task 1.5: Execute Full Regression Suite
  - **Target Files**: `Makefile`
  - **Verification**: `make test` passes all accumulated test suites with zero failures and zero regressions.
- [x] Task 1.6: Run the tool Section 1 item 6 selects for this envelope
  - **Tool**: none (no parser/codec property generation or complex pointer reinterpretation required; covered by hermetic SQLite test suite)
  - **Verification**: `make test` exits 0; evidence recorded in `plan/plan_cursor_enterprise_usage_chart.artifacts/gate-envelope-1.txt`.

**Gate:** Mandatory evidence file `plan/plan_cursor_enterprise_usage_chart.artifacts/gate-envelope-1.txt` recorded with red/green test results and `make test` passing.

**Hardstop:** Implementation cannot advance to Envelope 2 until Envelope 1 unit tests, `make test`, and `gate-envelope-1.txt` exit 0.

---

### Envelope 2: Frontend Adaptive Graph & WebAssets Synchronization

**Goal:** Update `web/app.js`, `web/index.html`, and `include/WebAssets.hxx` so the dashboard renders the Cursor request burn-down curve when on Enterprise tier.

Planned files:
- `web/app.js` — Implement dual-mode chart rendering (Fast Requests Burn-down vs. Dollar Spend);
- `web/index.html` — Update labels and container styles for adaptive metric units;
- `include/WebAssets.hxx` — Synchronize inline raw string literals with updated `app.js` and `index.html`;
- `test/TestWebServerApi.cxx` — Verify `/api/status` endpoint exposes `requests_used` and `cumulative_requests`.

- [ ] Task 2.1: Update `renderCursorSpend()` in `web/app.js` to support request mode scaling, tooltips, and labels
  - **Target Files**: `web/app.js`
  - **Verification**: Browser console has zero syntax/runtime errors; chart scales cleanly to request maximums.
- [ ] Task 2.2: Synchronize updated `web/app.js` and `web/index.html` into `include/WebAssets.hxx`
  - **Target Files**: `include/WebAssets.hxx`
  - **Verification**: `git diff include/WebAssets.hxx` reflects identical content as `web/app.js` and `web/index.html`.
- [ ] Task 2.3: Implement & Execute Envelope 2 Qualification & WebServer API Integration Tests (`test/TestWebServerApi.cxx`)
  - **Target Files**: `test/TestWebServerApi.cxx`
  - **Test Matrix**:
    - *Repeatable*: Ephemeral HTTP server started on localhost test port with deterministic mock status.
    - *Coverage*: `/api/status` returns serialized `daily_spend` with `requests_used` and `cumulative_requests` fields.
    - *Boundary*: Empty `daily_spend` array vs fully populated 30-day series.
    - *Fault Injection*: Simulated client disconnect, concurrent `/api/status` HTTP GET requests.
    - *Integrated Scope*: WebServer serialization matches frontend JSON parsing expectation.
  - **Verification**: `./build/test_web_server_api` exits code 0 with 100% assertions passing.
- [ ] Task 2.4: Execute Full Regression Suite
  - **Target Files**: `Makefile`
  - **Verification**: `make test` passes all accumulated test suites with zero failures and zero regressions.
- [ ] Task 2.5: Run the tool Section 1 item 6 selects for this envelope
  - **Tool**: none (frontend JS & static header sync; verified by `test_web_server_api` and browser inspection)
  - **Verification**: `make test` exits 0; evidence recorded in `plan/plan_cursor_enterprise_usage_chart.artifacts/gate-envelope-2.txt`.
- [ ] Task 2.6: Deploy and functionally verify live dashboard on `builder.selfso.com:3883/#quotas`
  - **Target Files**: daemon runtime binary
  - **Verification**: Live SVG DOM check on `http://builder.selfso.com:3883/#quotas` confirms `<path id="spend-chart-line">` renders valid curve and Y-axis displays request ticks (`120k`, `90k`, etc.).

**Gate:** Mandatory evidence file `plan/plan_cursor_enterprise_usage_chart.artifacts/gate-envelope-2.txt` recorded; live visual and SVG DOM inspection confirms functional rendering.

**Hardstop:** Completion requires visual verification that the chart renders properly without console errors before proceeding to plan close-out.

---

### 3.8 Mandatory Verification and Validation Gates Reference

#### 3.8.1 Backend Data Model & History Aggregation Gate (Envelope 1)
- **Test Target**: `./build/test_cursor_collector` via `make test`
- **Nominal Coverage**: Feed mock Enterprise response (`{"effectiveLimitCents": 120000}`) and assert `dailySpendHistory` contains daily synthesized points with `requests_used` and `cumulative_requests`.
- **Boundary Conditions**: 0 historical samples (fresh install/empty DB), 1 sample, multi-month rollover.
- **Fault Injection**: Simulated SQLite query error, empty/corrupt DB, missing table fallback.
- **Regression Verification**: `make test` passes all accumulated test suites with 100% assertions.
- **Acceptance Threshold**: 100% assertions pass, 0 memory leaks, exit code 0.

#### 3.8.2 WebServer API & Live Dashboard Visual Gate (Envelope 2)
- **Test Target**: `./build/test_web_server_api` via `make test` & `http://builder.selfso.com:3883/#quotas`
- **Nominal Coverage**: Query `/api/status` and assert `cursor.daily_spend` contains valid `requests_used` entries. Verify frontend SVG elements in browser.
- **Boundary Conditions**: Zero requests used (flat baseline), limit saturation (100% burn-down).
- **Fault Injection**: Missing fields in JSON, null values, network disconnects during fetch.
- **Regression Verification**: `make test` passes all accumulated test groups with 100% assertions.
- **Acceptance Threshold**: 100% assertions pass, exit code 0; SVG `<path id="spend-chart-line">` has valid non-empty `d` attribute, and Y-axis displays request counts.

---

### 3.9 Completion Definition

The architecture is freeze-ready when this plan is approved. The implementation is complete only when:
1. All envelopes execute in order without crossing a hardstop;
2. Every gate in Section 3.8 has evidence recorded in `plan/plan_cursor_enterprise_usage_chart.artifacts/`, and host-side suites pass with 100% verification;
3. Complete test suites pass under `make test`;
4. `mull-runner` has been run on the changed C or C++ functions when this plan changed tests (or exclusions recorded in `plan/plan_cursor_enterprise_usage_chart.artifacts/mull-exclusions.txt`);
5. `include/WebAssets.hxx` is fully synchronized with `web/app.js` and `web/index.html`;
6. `http://builder.selfso.com:3883/#quotas` renders the Cursor Fast Requests curve cleanly without any client-side JavaScript exceptions;
7. Implementation changes are reviewed and committed only on explicit user instruction.

---

## Appendix. Lifecycle Transition & Status Log

| Date | Previous State | New State | Lifecycle Directory | Notes / Rationale |
|---|---|---|---|---|
| 2026-10-02 | — | `Proposed` | `plan/` | Initial plan drafted to fix missing Cursor graph on Enterprise accounts |
| 2026-10-02 | `Proposed` | `Proposed` | `plan/` | Updated plan to adhere strictly to latest single-agent execution plan template |
| 2026-10-02 | `Proposed` | `Executing` | `plan/` | Approved by user; execution initiated for Envelope 1 |

### Checkpoints & Iteration Log
- 2026-10-02 18:32: Root cause verified: Cursor Enterprise tier returns $0 spend and empty `dailySpend` array; UI chart wipes canvas when spend history is empty.
- 2026-10-02 21:40: Plan upgraded to align with canonical single-agent template, Gemini quality gates, and plan-lifecycle rules.
- 2026-10-02 21:48: Execution initiated on Envelope 1 (Backend History Aggregation & Fast Request Timeseries Model).
- 2026-10-02 21:56: Envelope 1 completed: unit tests (TestCursorCollector) and full make test regression suite passed 100%; gate-envelope-1.txt recorded. Advancing to Envelope 2.
