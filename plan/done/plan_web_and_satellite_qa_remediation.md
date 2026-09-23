# Architectural Specification & Implementation Plan: Web Dashboard & Satellite Systems QA Remediation

- **Author**: Charles Chiou
- **Date**: 2026-09-23
- **Status**: Executed & Verified (100% Pass across all 4 tracks)
- **Scope**:
  - `aimon` Web Dashboard & Gateway Hub (`web/index.html`, `web/style.css`, `web/app.js`, `src/WebServer.cxx`).
  - `netmon` Satellite (`netmon/web/app.js`, `netmon/web/style.css`).
  - `meshmon` Satellite (`meshmon/web/style.css`, `meshmon/web/index.html`, `meshmon/web/app.js`).
  - `embdevenv` Satellite (`embdevenv/src/WebServer.cxx`).
  - Automated E2E Headless Browser Verification (`scripts/verify_e2e_ui.mjs`).

---

## 1. Executive Summary & Problem Diagnosis

Following a comprehensive 4-agent automated QA audit across the unified `aimon` gateway hub and all reachable satellite monitoring nodes on `selfso.com` (`netmon`, `meshmon`, `embdevenv`), several usability, chart scaling, viewport responsiveness, and interaction safety deficiencies were identified:

1. **Aimon Latency Chart Distortion & Usability (P0 Critical)**:
   - "Step Duration & Latency Envelope" chart lacks colored legend indicators matching SVG curves (`#3b82f6` Blue for Average, `#f59e0b` Amber for P95).
   - Mislabels P95 as "Max Duration".
   - Outlier spikes (up to 330,000 ms) flatten the average line into a 0.0 ms baseline.
   - Raw floats clip against the left boundary due to narrow padding (`padL = 50`).
   - Missing X-axis temporal labels, missing hover tooltips, and lack of subtitle explaining latency health thresholds.

2. **Aimon Workload Velocity Chart (P1 High)**:
   - Tool calls/s (0.0–4.6) and active agent sessions (1–3) share a single linear Y-axis, rendering confusing fractional sessions like `2.3`. Requires dual-axis scaling.

3. **Mobile & Viewport Breakpoint Flaws (P1 High)**:
   - Top navbar timestamp wraps into 3 lines on mobile viewports (< 600px).
   - `.monitor-nav` tabs overflow without horizontal touch scrolling.
   - Cursor card "Days Remaining" box wraps words vertically.
   - Cursor spend chart date labels collide on small screens.
   - Tool matrix progress bar collapses to 0 width.
   - Iframe container height produces dual nested vertical scrollbars on laptops (1366x768).

4. **DOM Polish, Favicon & Polling Overhead (P2/P3)**:
   - Dead DOM lookup `#kpi-token-velocity` in `web/app.js`.
   - Generic placeholder displayed when active session has 0 events.
   - Truncated tool matrix rows lack hover tooltips.
   - Missing `/favicon.ico` returns HTTP 404.
   - Polling frequency causes 177 requests / 30s.

5. **Satellite Systems Deficiencies (NetMon, MeshMon, EmbDev)**:
   - **NetMon**: WAN canvas charts lack hover crosshair tooltips; LAN table lacks pagination for 137 devices.
   - **MeshMon**: 9-tab sub-navigation wraps into 4 uneven lines on mobile; tab badges render unescaped whitespace newlines (`Live Packet Sniffer\n 0`); locked automation buttons lack guidance.
   - **EmbDev**: Hardware power-off / reboot actions lack safety modal protection; serial terminal lacks focus cues and mobile scroll wrapping.

---

## 2. Detailed Technical Solutions & Architectural Specifications

### 2.1 Track 1: Aimon Multi-Monitor Hub & Telemetry Gateway (`builder`)

#### A. Latency & Envelope Chart Overhaul (`web/index.html`, `web/app.js`)
- **Visuals & Legend**:
  ```html
  <div class="chart-header">
      <div class="chart-title">Step Duration & Latency Envelope</div>
      <div class="chart-legend">
          <span class="legend-item"><i class="dot dot-blue"></i> Avg Duration</span>
          <span class="legend-item"><i class="dot dot-amber"></i> P95 Envelope</span>
      </div>
  </div>
  <p class="chart-subtitle">P95 envelope tracks 95th percentile upper variance. Latency health: &lt; 3s Normal (Green), 3–8s Moderate (Amber), &gt; 8s Degraded (Red).</p>
  ```
- **Dynamic Y-Axis Formatting & Padding**:
  - Set `padL = 65`, `padR = 25`, `padT = 20`, `padB = 35`.
  - Format values dynamically:
    - `< 1,000 ms`: `${v.toFixed(0)} ms`
    - `1,000 .. 59,999 ms`: `${(v / 1000).toFixed(1)}s`
    - `>= 60,000 ms`: `${(v / 60000).toFixed(1)}m`
- **X-Axis Temporal Reference**:
  - Generate 4–5 evenly spaced time tick labels along the bottom baseline using the `timestamps` array (`Date.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })`).
- **Interactive Hover Crosshair & Floating Tooltip**:
  - Add mousemove / mouseleave listener on SVG chart container with a vertical guide line `<line class="chart-crosshair">` and `<g class="chart-tooltip">` displaying timestamp, Average Latency, and P95 Envelope.

#### B. Workload Velocity Dual Y-Axis Chart (`web/app.js`)
- Left Axis: `Tool Invocations (calls/s)` in Cyan (`#38bdf8`), formatted as `${v.toFixed(1)} calls/s`.
- Right Axis: `Active Sessions` in Magenta (`#e879f9`), formatted with clean integer steps (`0, 1, 2, 3+`), `padR = 45`.
- X-axis temporal ticks and hover tooltip showing calls/s and session count.

#### C. Responsive Viewport & Breakpoint Hardening (`web/style.css`)
- **Monitor Tabs**:
  ```css
  .monitor-nav {
      overflow-x: auto;
      white-space: nowrap;
      -webkit-overflow-scrolling: touch;
      scrollbar-width: thin;
      flex-wrap: nowrap;
  }
  ```
- **Iframe Sizing on Laptops**:
  ```css
  .frame-content-wrapper, .monitor-iframe {
      height: calc(100vh - 150px);
      min-height: 520px;
  }
  ```
- **Mobile Navbar & Progress Tracks**:
  - `.last-updated`: `white-space: nowrap; font-size: 0.76rem;`
  - `.details-box`: `min-width: 130px;`
  - `.tool-bar-track`: `min-width: 60px; flex: 1 1 auto;`
  - `.waterfall-turn-row .badge`: `max-width: 100%; text-overflow: ellipsis; overflow: hidden; font-size: 0.72rem;`

#### D. DOM Cleanup, Favicon & Polling Optimization (`web/app.js`, `src/WebServer.cxx`)
- Remove dead `document.getElementById('kpi-token-velocity')` lookup.
- When an active session has 0 events, render:
  `<div class="waterfall-empty-hint">Session <code>${escapeHtml(sessionId)}</code> is active, but 0 lifecycle events recorded yet.</div>`
- Add `title="${escapeHtml(t.tool_name)}"` to tool matrix rows.
- Serve inline SVG favicon on `GET /favicon.ico` in `src/WebServer.cxx`.
- Back off background polling intervals from 3s to 15s.

---

### 2.2 Track 2: NetMon Bandwidth Charts & LAN Table Pagination (`rhino` / `netmon`)

#### A. WAN Traffic Canvas Hover Tooltips (`netmon/web/app.js`)
- Attach `mousemove`, `mouseleave`, and `touchmove` event handlers to `canvas-ppp11` and `canvas-ppp12`.
- Project mouse X-coordinate into time series array index.
- Draw vertical crosshair guide and render floating HTML tooltip overlay showing exact timestamp, Inbound Mbps (Cyan), and Outbound Mbps (Magenta).

#### B. LAN Devices Table Pagination (`netmon/web/app.js`, `netmon/web/style.css`)
- Implement client-side pagination state: `currentPage = 1`, `pageSize = 25` (selectable: 25, 50, 100, All).
- Render pagination toolbar below the table:
  - `<button class="btn-page" id="btn-prev">‹ Prev</button>`
  - `<span class="page-info">Page 1 of 6 (137 devices)</span>`
  - `<button class="btn-page" id="btn-next">Next ›</button>`
  - `<select id="select-page-size"><option value="25">25 / page</option><option value="50">50 / page</option><option value="100">100 / page</option><option value="0">All</option></select>`
- Smoothly slice filtered/sorted devices array before DOM insertion.

#### C. Polish & Mobile Layout (`netmon/web/style.css`, `netmon/web/app.js`)
- Informative state for empty Top Bandwidth Hosts (15m): *"No active network flows exceeding 1 KB/s in last 15 minutes."*
- Adjust WAN card grid columns on mobile screens (< 400px).

---

### 2.3 Track 3: MeshMon Mobile Sub-Nav & Tab Formatting (`fox` / `meshmon`)

#### A. Horizontal Touch Scrolling Sub-Nav (`meshmon/web/style.css`)
- Add `overflow-x: auto; white-space: nowrap; -webkit-overflow-scrolling: touch; scrollbar-width: thin; flex-wrap: nowrap;` to `.tabs-nav` and `.tabs-row` so the 9 tabs scroll smoothly on mobile instead of wrapping into 4 uneven lines.

#### B. Tab Badge Whitespace Cleanliness (`meshmon/web/index.html`, `meshmon/web/app.js`)
- Fix template whitespace so buttons render as `Live Packet Sniffer (0)` without unescaped line breaks.

#### C. Locked Automation Button Guidance (`meshmon/web/app.js`)
- When clicking `.btn-auth-guarded` in View-Only mode, display a floating toast / tooltip: *"Click 'View Only' in the top navbar to authenticate and unlock automation controls."*

---

### 2.4 Track 4: EmbDev Hardware Safety & Terminal Usability (`rhino` / `embdevenv`)

#### A. Safety Confirmation for Power Actions (`embdevenv/src/WebServer.cxx`)
- Ensure `■ Power OFF` and `↻ Reboot` trigger `safetyModal` with explicit confirmation buttons before issuing MCU commands.

#### B. Terminal Focus Cue & Mobile Wrapping (`embdevenv/src/WebServer.cxx`)
- Add visual focus border highlight and helper text: *"Click outside or press Esc to unfocus terminal."*
- Wrap xterm.js container in an `overflow-x: auto;` wrapper for narrow mobile screens.

---

### 2.5 Track 5: Hierarchical Multi-Monitor Navigation Restructure

#### A. Top-Level Peer Navigation Bar (`#monitor-tabs`)
- Top bar contains strictly the 4 peer monitoring systems on `selfso.com`:
  - `aimon` (Central AI Monitoring & Gateway Hub, `127.0.0.1:3883`)
  - `netmon` (Network Telemetry & Sniffer Gateway, `192.168.8.30:3884`)
  - `meshmon` (Meshtastic LoRa RF Gateway, `192.168.8.245:16880`)
  - `embdevenv` (Embedded Development & Console Gateway, `192.168.8.30:3886`)

#### B. Internal Aimon Sub-Navigation Tab Bar & Subpanels (`#view-aimon`)
- When `aimon` is active, it presents an internal horizontal touch-scrollable sub-navigation bar (`.aimon-subnav-bar`):
  - `AI Quotas` (`#subpanel-quotas`): Antigravity & Cursor quota gauges, cumulative spend breakdown, MCP active sessions.
  - `Agent Telemetry` (`#subpanel-telemetry`): 1H/24H/7D/30D/1Y timeframes, KPI summary cards, dual-axis velocity chart, latency envelope chart, category matrix, lifecycle waterfall.
  - `Action Approvals` (`#subpanel-approvals`): Mobile pairing SVG QR matrix, secret regenerator, pending approval cards, paired device list.
- Dynamic URL hash router (`#aimon/quotas`, `#aimon/telemetry`, `#aimon/approvals`, `#netmon`, `#meshmon`, `#embdevenv`) preserves bookmarkability and direct subpanel linking.

---

## 3. Staged Implementation Roadmap

```
 ┌────────────────────────────────────────────────────────────────────────────┐
 │ STAGE 1: Central Aimon Hub & Telemetry Gateway Usability Overhaul          │
 │   - web/index.html, web/style.css, web/app.js, src/WebServer.cxx           │
 │   ► Gate 1: C++ unit tests + daemon reload + 1080p & mobile visual verify  │
 └─────────────────────────────────────┬──────────────────────────────────────┘
                                       │
                                       ▼
 ┌────────────────────────────────────────────────────────────────────────────┐
 │ STAGE 2: NetMon Canvas Hover Tooltips & LAN Table Pagination               │
 │   - netmon/web/app.js, netmon/web/style.css                                │
 │   ► Gate 2: Compile & restart netmon on rhino, verify interactive canvas   │
 └─────────────────────────────────────┬──────────────────────────────────────┘
                                       │
                                       ▼
 ┌────────────────────────────────────────────────────────────────────────────┐
 │ STAGE 3: MeshMon Mobile Sub-Nav & Tab Formatting                           │
 │   - meshmon/web/style.css, meshmon/web/index.html, meshmon/web/app.js      │
 │   ► Gate 3: Compile on fox & restart meshmon, verify touch scrolling tabs  │
 └─────────────────────────────────────┬──────────────────────────────────────┘
                                       │
                                       ▼
 ┌────────────────────────────────────────────────────────────────────────────┐
 │ STAGE 4: EmbDev Safety Modals & Terminal Usability                         │
 │   - embdevenv/src/WebServer.cxx                                            │
 │   ► Gate 4: Compile on rhino, verify safety confirmation modal             │
 └─────────────────────────────────────┬──────────────────────────────────────┘
                                       │
                                       ▼
 ┌────────────────────────────────────────────────────────────────────────────┐
 │ STAGE 5: Hierarchical Multi-Monitor Navigation Restructure                 │
 │   - Top-level: aimon, netmon, meshmon, embdevenv                           │
 │   - Aimon internal subpanels: AI Quotas, Agent Telemetry, Action Approvals │
 │   ► Gate 5: 0 JS errors, 0 404s, perfect multi-system responsiveness       │
 └────────────────────────────────────────────────────────────────────────────┘
```

---

## 4. Verification Plan

### Automated C++ Builds & Unit Tests
```bash
# 1. aimon (builder)
make clean && make -j$(nproc)
make test

# 2. netmon (rhino)
ssh -n rhino "cd ~/work/netmon && make -j$(nproc)"

# 3. meshmon (fox)
ssh -n fox "cd ~/work/meshmon && make -j$(nproc)"

# 4. embdevenv (rhino)
ssh -n rhino "cd ~/work/embdevenv && make -j$(nproc)"
```

### Daemon Restarts Inside Screen Sessions
```bash
# Local aimon on builder
screen -S aimon -X stuff $'\003'
screen -S aimon -X stuff "./build/aimon daemon --port 3883\n"

# Remote netmon on rhino
ssh -n rhino "screen -S netmon -X stuff \$'\003'"
ssh -n rhino "screen -S netmon -X stuff './build/netmon --daemon\n'"

# Remote meshmon on fox
ssh -n fox "screen -S meshmon -X stuff \$'\003'"
ssh -n fox "screen -S meshmon -X stuff './build/aarch64/meshmon\n'"
```

### Headless Browser Automation Suite
```bash
node --experimental-websocket scripts/verify_e2e_ui.mjs
node --experimental-websocket scripts/verify_netmon_e2e.mjs
node --experimental-websocket scripts/verify_meshmon_e2e.mjs
node --experimental-websocket scripts/verify_embdev_e2e.mjs
```
- Capture 1080p Desktop and 375x812 Mobile screenshots for:
  - `aimon` Top navigation with peer monitors (`aimon`, `netmon`, `meshmon`, `embdevenv`).
  - `aimon` Internal subpanels (`AI Quotas`, `Agent Telemetry`, `Action Approvals`).
  - `netmon` WAN Traffic Charts with hover tooltips and Paginated LAN Table.
  - `meshmon` Touch-scrollable Sub-Nav and Clean Badges.
  - `embdevenv` Safety Modal and ANSI Terminal.
- Verify 0 JavaScript errors and 0 HTTP 404 errors across all endpoints.

