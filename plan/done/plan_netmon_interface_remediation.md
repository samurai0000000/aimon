# Architectural Specification & Implementation Plan: NetMon Interface Remediation & Visual Polish

- **Author**: Charles Chiou
- **Date**: 2026-09-23
- **Status**: Ready for Execution
- **Target Repository**: `netmon` (`/home/samurai/work/netmon`) & `aimon` Hub (`/home/samurai/work/aimon`)
- **Target Deployment Host**: `rhino` (`192.168.8.30:3884`) & `builder` (`127.0.0.1:3883`)

---

## 1. Executive Summary & Defect Diagnosis

A deep automated browser inspection of the `netmon` web interface (both directly on `http://192.168.8.30:3884` and embedded within `aimon` at `http://127.0.0.1:3883`) identified several critical layout, rendering, responsiveness, and UX issues:

1. **Severe Mobile & Narrow Viewport Layout Breakage**:
   - `.wan-grid` hardcodes `grid-template-columns: repeat(auto-fit, minmax(580px, 1fr))`. On any viewport narrower than 600px (phones, small windows, split IDE viewports), WAN cards force a 580px minimum width, pushing the outbound rate, chart right edge, and navbar metadata pills completely off-screen with horizontal clipping.
   - Section headers (e.g. WAN Uplink Telemetry + SNMP badge + Timeframe buttons) lack flex wrapping and collision breakpoints, causing the timeframe buttons (Month, Year) and status pills (DB Size) to clip offscreen.
   - Inbound / Outbound rate cards clip text (`1.` instead of `1.09 Mbps`).

2. **Canvas Graph DPI Scaling, Resize Observer & Performance Issues**:
   - Canvas charts (`#canvas-ppp11`, `#canvas-ppp12`) do not register a resize observer or window resize listener. Resizing the browser or rotating a device leaves charts stretched, blurred, or stale.
   - On every mousemove event, `drawSmoothAreaChart()` was reallocating the canvas backing buffer dimensions (`canvas.width = rect.width * dpr`), causing unnecessary GC pressure and hover stutter.
   - Lack of touch event support (`touchstart`, `touchmove`, `touchend`) for mobile chart crosshairs and tooltips.

3. **Protocol Breakdown & Empty State Flaws**:
   - When network traffic is quiet or protocols have 0 samples, the calculation lumped 100% into "Other", painting the entire bar purple ("Other: 100.0%") and confusing users into thinking heavy unclassified traffic exists.
   - Top Bandwidth Hosts list rendered raw unstyled text with no visual polish.

4. **LAN Discovered Devices Table UX & Responsiveness**:
   - Table header (`<thead>`) is not sticky, losing context when scrolling through the 139 devices.
   - Table container lacked proper overflow styling and mobile cell padding.
   - Device search lacked clear feedback and instantaneous result counter sync.

5. **Double Nested Scrollbars in Aimon Hub**:
   - When embedded inside `aimon` (`.monitor-iframe`), outer container height plus internal page padding generated dual nested vertical scrollbars.

---

## 2. Detailed Technical Remediation Plan

### 2.1 Track 1: Responsive Layout & Mobile Breakpoint Hardening (`netmon/web/style.css`, `netmon/web/index.html`)

- **WAN Grid Flexibility**:
  ```css
  .wan-grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(min(100%, 460px), 1fr));
      gap: 1.25rem;
  }
  ```
- **Mobile Breakpoint Styles (`@media (max-width: 768px)`)**:
  - Navbar: Allow `.status-indicators` to wrap neatly or use compact pill formats.
  - Section Header: Flex-wrap headers so title and timeframe buttons / search inputs stack gracefully.
  - Rate Cards: `.wan-rate-row` adjusts grid spacing so rate numbers never truncate.
  - Meta Grid: `.wan-meta-grid` converts to 2-column grid (`repeat(2, 1fr)`) with compact padding.
  - Metrics Grid: `.metrics-grid` stacks cards single-column on phones.
  - Table: Smooth horizontal scrollable container with rounded borders and sticky column header.

### 2.2 Track 2: Canvas High-DPI Chart & Interactive Tooltip Engine (`netmon/web/app.js`)

- **Resize Observer & DPI Adaptation**:
  - Implement a `ResizeObserver` attached to each `.chart-container` to automatically resize canvas buffers and re-render time series curves without blur or distortion.
  - Cache rendered chart bitmaps or optimize hover crosshair draws to prevent canvas dimension resets on mouse moves.
- **Touch Interaction**:
  - Add `touchstart` / `touchmove` / `touchend` listeners to enable mobile users to scrub time series graphs and view tooltip data.
- **Timeframe Selector Instant Feedback**:
  - Update timeframe buttons to immediately trigger historical queries and redraw charts with active loading indicators.

### 2.3 Track 3: Protocol Breakdown & Top Talkers Modernization (`netmon/web/app.js`, `netmon/web/style.css`)

- **Idle / Low Traffic State**:
  - If total packet count is 0 or below threshold, display a clean muted gradient with "Idle / Monitoring LAN" status.
  - Include HTTPS (`#4facfe`), DNS (`#10b981`), SSH (`#f59e0b`), ARP (`#06b6d4`), HTTP (`#3b82f6`), Other (`#8b5cf6`).
- **Top Bandwidth Hosts Widget**:
  - Modern card with ranked badges (#1, #2, #3), device names, bandwidth rate, and subtle percentage fill bars.
  - Informative empty state: *"No active high-bandwidth flows in the last 15 minutes."*

### 2.4 Track 4: LAN Devices Table & Pagination Polish (`netmon/web/app.js`, `netmon/web/style.css`)

- **Sticky Header**:
  ```css
  .data-table thead th {
      position: sticky;
      top: 0;
      background: var(--bg-surface-elevated);
      z-index: 5;
  }
  ```
- **Enhanced Search & Sorting**:
  - Search filter matches Device Name, IP Address, MAC Address, OUI Vendor, and Category.
  - Clean sort indicators (▲ / ▼) with active column highlights.
  - Dynamic page size picker (25, 50, 100, All) and page navigation buttons.

### 2.5 Track 5: Aimon Hub Iframe Seamless Integration (`aimon/web/style.css`)

- Adjust `.monitor-frame-panel` and `.frame-content-wrapper` height to prevent double scrollbars on standard 1080p and 768p displays.

---

## 3. Files to Modify

1. **`netmon/web/style.css`**: Complete responsive overhaul, sticky table header, mobile cards, chart tooltips, glassmorphism tokens.
2. **`netmon/web/app.js`**: Chart resize observer, touch event handlers, protocol breakdown idle handling, table search/pagination sync.
3. **`netmon/web/index.html`**: Clean semantic structure, responsive meta tags, search bar accessibility.
4. **`netmon/include/WebAssets.hxx`**: Synchronize static fallback strings.
5. **`aimon/web/style.css`**: Optimize satellite iframe container geometry.

---

## 4. Verification & Testing Plan

1. **Automated E2E CDP Test Suite**:
   - Run `node --experimental-websocket scripts/audit_netmon.mjs` across:
     - Desktop Viewport (1920x1080)
     - Laptop Viewport (1366x768)
     - Tablet Viewport (768x1024)
     - Mobile Viewport (375x812)
2. **Interactive Functional Tests**:
   - Timeframe switching (Hour, Day, Week, Month, Year).
   - Chart hover crosshair and tooltip accuracy.
   - Device search filtering and column sorting.
   - Device table pagination (Page 1, 2, Page Size changes).
   - Direct NetMon access (`:3884`) and embedded Aimon tab access (`:3883`).
3. **Compilation & Screen Session Reload**:
   - Compile on `rhino`: `ssh -n rhino "cd ~/work/netmon && make -j$(nproc)"`.
   - Reload `netmon` daemon in screen session `netmon`.
