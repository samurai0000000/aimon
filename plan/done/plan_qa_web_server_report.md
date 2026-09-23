# QA Testing & Usability Audit Report: aimon Web Dashboard & Telemetry Gateway

## License & Copyright

Copyright (C) 2026, Charles Chiou. All rights reserved.

---

## Executive Summary

A coordinated 4-agent automated Quality Assurance (QA) and Usability Audit was performed against the `aimon` unified web server (running locally on port `3883`) using native headless Chromium automation. The audit exercised all views, navigation states, responsive viewport breakpoints (Desktop 1920×1080, Laptop 1366×768, Tablet 768×1024, Mobile 375×812), interactive controls, SVG charting engines, REST APIs, SSE telemetry streaming, and client runtime diagnostics.

The testing evaluated the dashboard across four specialized QA domains:
1. **Agent 1: Visual Design, UX Clarity & Chart Usability QA** (Focus: Design system, typography, chart readability, legends, axis units, user confusion points).
2. **Agent 2: Interactive Controls, Navigation & UX Flow QA** (Focus: Multi-monitor tabs, URL hash routing, collapsible model gauges, timeframe filtering, refresh mechanics, QR pairing flow).
3. **Agent 3: Data Integrity, Telemetry Accuracy & Calculations QA** (Focus: Metric accuracy, KPI calculations, timeseries aggregation, outlier distortion, quota countdowns).
4. **Agent 4: Client-Side Robustness, Network Overhead & Accessibility QA** (Focus: Browser console health, DOM validation, polling efficiency, SSE resilience, WCAG compliance).

---

## Agent 1: Visual Design, UX Clarity & Chart Usability QA

### 1.1 "Step Duration & Latency Envelope" Chart Usability & Design Flaws
*Status: Critical Usability Issue (Direct User Confusion Point)*

#### Identified Deficiencies:
1. **Missing Colored Legend Swatches**: The chart header displays the text `"Avg Duration (ms)"` and `"Max Duration (ms)"` in plain, unstyled gray text without colored indicator dots, lines, or badges matching the SVG paths (`#3b82f6` Blue for Average, `#f59e0b` Amber for P95/Max). A user looking at the chart cannot determine which line represents which metric.
2. **Data vs. Legend Discrepancy (P95 vs. Max)**: The backend timeseries API (`/api/telemetry/timeseries`) supplies `p95_turn_latency_ms` (the 95th percentile latency), but the UI legend labels it `"Max Duration (ms)"`. If this is 95th percentile, it should be explicitly labeled `P95 Envelope (ms)` or `Peak Latency (ms)` with clear definitions.
3. **Outlier Linear Scale Distortion (Flattening Average Curve)**: The chart calculates the Y-axis ceiling using a simple linear max: `maxVal = Math.max(...avg, ...p95) * 1.15`. When a single long-running tool step (such as `write_to_file` or `replace_file_content`) spikes to 164,000–330,000 ms (164–330 seconds), the Y-axis ceiling expands to `331,200 ms`. Consequently, the normal average turn duration (averaging ~4,000 ms / 4.0s) is compressed into a flat line indistinguishable from `0.0 ms` at the bottom of the chart. The user cannot observe day-to-day average latency variations.
4. **Y-Axis Label Text Truncation and Clipping**: The Y-axis values are rendered as raw unformatted floats with up to 8 characters (e.g. `331200.0`, `248400.0`, `165600.0`, `82800.0`). Because the SVG left padding is fixed (`padL = 50`), these wide number strings are clipped at the left chart boundary, displaying truncated numbers like `31200.0` or `82800.0`.
5. **Completely Absent X-Axis (No Time / Date Reference)**: The chart provides no horizontal axis labels, no time stamps, no date markers, and no vertical tick marks. Users cannot discern whether a latency spike occurred 10 minutes ago, 4 hours ago, or yesterday.
6. **Zero Hover Interactivity or Tooltips**: Unlike the Cursor spend chart which features interactive dots and hover titles, the telemetry SVG chart paths lack hover inspection points, crosshairs, or tooltips showing the exact timestamp and duration.
7. **Lack of User Explanation / Context**: The chart title `"Step Duration & Latency Envelope"` lacks any subtitle or help tooltip explaining:
   - What an "Envelope" means (the spread between mean turn duration and upper percentile variance).
   - What constitutes an acceptable latency threshold (e.g. green < 3s, yellow 3–8s, red > 8s).

---

### 1.2 "Tool Invocations & Workload Velocity" Chart Usability
*Status: High Usability Issue*

#### Identified Deficiencies:
1. **Unrelated Dimensions Plotted on a Single Y-Axis**: The chart plots `Tool Invocations (calls/s)` (rate metric: 0.0 to 4.6 calls/sec) and `Active Sessions` (count metric: 1 to 3 concurrent agents) on the exact same linear Y-axis scale (`0.0`, `1.1`, `2.3`, `3.4`, `4.6`). Fractional labels such as `2.3` and `3.4` are nonsensical for active agent session counts, and no axis unit labels specify whether the number represents calls/sec or agent counts.
2. **Missing Dual-Axis Scaling or Normalization**: Without a secondary right-side Y-axis or stacked sub-charts, the correlation between workload rate and concurrent agent count is visually muddled.
3. **Missing X-Axis Time Ticks and Hover Tooltips**: Same deficiency as Chart 1.1—no temporal grounding.

---

### 1.3 Viewport Responsiveness & Mobile Breakpoint Flaws
*Status: Moderate Usability Issue (Mobile / Tablet)*

Tested on Mobile (375×812) and Tablet (768×1024):
1. **Top Navbar Text Overlap**: On mobile (< 400px width), the `"Updated: HH:MM:SS AM"` timestamp wraps into a vertical stack of three lines directly adjacent to the `"Unified AI Quotas"` badge, squeezing against the Refresh button.
2. **Monitor Navigation Tab Truncation**: Navigation tabs (`AI Quotas`, `Agent Telemetry`, `Action Approvals`, `NetMon`, `MeshMon`, `EmbDev`) overflow horizontally without an explicit horizontal scrollbar or swipe indicator, causing satellite monitor tabs to disappear off-screen.
3. **Cursor "Days Remaining" Box Layout Break**: On mobile, the Cursor card right box renders `"8 days left"` as a vertical stack (`8`, `days`, `left`) due to narrow column flex-wrapping.
4. **Cursor Cumulative Spend X-Axis Collision**: Date labels (`Sep 1`, `Sep 6`, `Sep 11`, `Sep 16`, `Sep 21`) collide into an overlapping string `SepSepSepSepSep 1 6 11 16 21`.
5. **Tool Matrix Progress Bar Collapse**: On mobile viewports, the horizontal progress track bar (`.tool-bar-track`) collapses to 0 width due to unconstrained text columns.
6. **Waterfall Event Badge Overflow**: In the Agent Waterfall panel, the badge `"RECENT 100 / 14470 EVENTS"` overflows its cyan pill container on screens under 500px width.

---

### 1.4 Accessibility (a11y) & Color Contrast
*Status: Compliance & Usability Issue*

1. **Low Contrast on Muted Text**: Secondary labels (`.kpi-sub`, `.badge-sub`, `.spend-desc`) styled in `#6b7280` on the dark background `#0a0e17` yield a contrast ratio of ~3.2:1, failing WCAG AA requirements (4.5:1 minimum for regular text). Recommended fix: elevate muted text to `#9ca3af` (5.8:1 ratio).
2. **Missing SVG Accessible Labels**: SVG charts (`#latency-svg`, `#token-svg`) lack `role="img"` and `aria-label` descriptions for screen readers.

---

## Agent 2: Interactive Controls, Navigation & UX Flow QA

### 2.1 Tab Navigation & URL Hash Routing
*Status: Verified Functional with Minor Polish Needed*

- **Tab Switching**: Smooth switching across `AI Quotas` (`#aimon`), `Agent Telemetry` (`#telemetry`), and `Action Approvals` (`#approvals`). URL hash updates synchronously and page reload on `#telemetry` or `#approvals` correctly displays the target panel.
- **Dynamic Monitor Integration**: Satellite tabs (`NetMon`, `MeshMon`, `EmbDev`) dynamically register and display remote iframes.
- **Improvement Needed**: If a remote satellite host is unreachable, the greyed-out offline overlay is displayed, but iframe loading does not feature a progress skeleton while connecting.

---

### 2.2 Live Agent Lifecycle Waterfall Dropdown Flow
*Status: Functional Glitch Identified*

- **Empty State on Initial Load**: When navigating to the Telemetry tab, the session select dropdown automatically populates and selects the first active session (`sess-ce561280`). However, the timeline container below remains showing the empty placeholder message:
  `"Select a session above to inspect turn breakdown, tool executions, and approval wait times."`
- **Root Cause**: `fetchSessionWaterfall(chosenSessionId)` succeeds, but when a session has zero turns/events, the UI does not distinguish between *"No session selected"* and *"Session selected, but 0 execution events recorded"*.

---

### 2.3 Interactive Buttons & Controls
*Status: Functional*

- **Refresh Button**: Successfully triggers `/api/refresh`, adds the `.spinning` CSS animation class to the SVG icon, and completes within 500ms.
- **Individual Model Capacities Toggle**: Smoothly expands and collapses the 14 circular gauges; chevron icon rotates appropriately.
- **Timeframe Selector (1H, 24H, 7D, 30D, 1Y)**: Correctly toggles the `.active` class, updates `activeTelemetryWindow`, and re-queries `/api/telemetry/timeseries?window=...`.
- **Mobile Companion Pairing QR**: Successfully generates and renders QR SVG via `qrcode.js` and updates the 300s countdown timer.
- **Improvement Needed**: Clicking `"Regenerate Secret"` does not provide a temporary loading spinner or visual flash on the secret text to confirm that a new secret was minted.

---

## Agent 3: Data Integrity, Telemetry Accuracy & Calculations QA

### 3.1 Dead DOM Reference in `app.js`
*Status: Bug in Frontend JavaScript*

- In `web/app.js` lines 913 & 921:
  ```javascript
  const tokenVelocityEl = document.getElementById('kpi-token-velocity');
  if (tokenVelocityEl) {
      tokenVelocityEl.innerHTML = `${(data.total_tool_calls || 0).toLocaleString()} <span class="kpi-sub">calls</span>`;
  }
  ```
- **Finding**: `#kpi-token-velocity` does not exist in `web/index.html` (the four KPI cards are `#kpi-active-sessions`, `#kpi-total-tools`, `#kpi-avg-latency`, `#kpi-error-rate`). Furthermore, the code assigns `total_tool_calls` to this dead variable, duplicating the assignment of `#kpi-total-tools`.

---

### 3.2 Tool Invocation Name Duplication & Formatting
*Status: Telemetry Telemetry Aggregation Polish*

- **Duplicate Tool Names**: The Tool Invocations matrix lists `list_dir` (424 calls, avg 2375 ms) and `list_directory` (424 calls, avg 1215 ms) as two separate tools. These represent the same logical file-listing operation across different agent implementations.
- **Ellipsis Clipping Without Tooltips**: Long tool names such as `multi_replace_file_content` are truncated with `...` (`multi_replace_file_co...`) without a full-name tooltip on hover.

---

### 3.3 Antigravity & Cursor Quota Data Fidelity
*Status: Highly Accurate*

- **Antigravity Parsing**: Correctly parses Google One AI credits (`3,024`), prompt & flow balance (`500 / 100`), monthly allowances (`50k / 150k`), and group quota buckets (`Gemini Models: 88% weekly, 93% 5-hour; Claude & GPT: 60% weekly, 100% 5-hour`).
- **Circular Gauges**: 14 individual models render accurate percentage offsets and color thresholding (Green > 50%, Amber 20–50%, Red < 20%).
- **Cursor Spend**: Spend chart accurately handles cumulative spend ($900.53), auto-calculates Y-axis bounds ($950 max), formats date labels, and parses spend-by-model breakdown categories summing to 100%.

---

## Agent 4: Client-Side Robustness, Network Overhead & Accessibility QA

### 4.1 Client-Side Network Request Thrashing (Polling Frequency)
*Status: Architecture Optimization Recommended*

During a 30-second session, the client issued **177 HTTP requests** across multiple concurrent `setInterval` timers:
- `/api/status`: Every 5,000 ms
- `/api/sessions`: Every 3,000 ms
- `/api/monitors`: Every 8,000 ms
- `/api/telemetry/*`: Every 3,000 ms (when active on Telemetry tab)
- `/api/approvals/*`: Every 3,000 ms (when active on Approvals tab)

#### Recommendation:
Since the application establishes an active Server-Sent Events (SSE) stream on `/sse`, the frontend should rely on SSE push notifications (`tools_changed`, `session_updated`, `approval_requested`) for real-time updates and back off periodic polling to 15–30 seconds to minimize CPU and network overhead.

---

### 4.2 Missing Favicon
*Status: Minor 404 Error*

- The browser requests `GET /favicon.ico`, which returns HTTP 404 (`text/plain`).
- **Fix**: Provide an embedded SVG or PNG favicon route in `WebServer.cxx`.

---

## Summary of Priority Recommendations & Action Items

| Priority | Component | Issue Description | Proposed Solution |
|---|---|---|---|
| **P0 (Critical)** | **Latency Chart** | "Step Duration & Latency Envelope" is confusing, missing color legend indicators, mislabels P95 as Max, and outlier spikes flatten the average line. | 1. Add color swatch badges to legend (`#3b82f6` Average, `#f59e0b` P95 Peak).<br>2. Rename "Max Duration" to "P95 Envelope (ms)".<br>3. Format Y-axis numbers to human-readable units (`1.2s`, `45s`, `2.5m`).<br>4. Add time/date labels on the X-axis.<br>5. Add hover tooltips and an explanatory subtitle explaining latency health thresholds. |
| **P1 (High)** | **Workload Chart** | Dual dimensions (`calls/s` and `active sessions`) sharing a single Y-axis with confusing decimal numbers. | 1. Implement dual Y-axes (left for `calls/s`, right for `sessions`) or normalize series.<br>2. Add unit labels and X-axis timestamps. |
| **P1 (High)** | **Mobile Layout** | Header, tab bar, date labels, and tool matrix break on narrow screens (< 600px). | 1. Enable horizontal touch scrolling on `.monitor-nav` with scroll hints.<br>2. Adjust flex wrap and font sizes for mobile viewports.<br>3. Skip intermediate dates on mobile spend chart to prevent text overlap. |
| **P2 (Medium)** | **JavaScript Engine** | Dead DOM element `#kpi-token-velocity` in `app.js`. | Remove dead DOM lookup and clean up duplicate variable assignment. |
| **P2 (Medium)** | **Waterfall Panel** | Shows generic placeholder when a selected session has 0 events. | Update container to display *"Session active, 0 lifecycle events recorded yet"*. |
| **P3 (Low)** | **Polling Overhead** | Excessive polling frequency (177 requests / 30s). | Leverage existing SSE stream for reactive updates and reduce polling interval to 15s. |
| **P3 (Low)** | **Web Server** | Missing `/favicon.ico` returning 404. | Serve inline SVG icon favicon from `WebServer.cxx`. |

---
