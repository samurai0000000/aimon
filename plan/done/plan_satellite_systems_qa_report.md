# Comprehensive QA & Usability Audit Report: Reachable Satellite Systems & Multi-Monitor Gateway

## License & Copyright

Copyright (C) 2026, Charles Chiou. All rights reserved.

---

## Executive Summary

An extensive Quality Assurance (QA) and Usability Audit was conducted across all reachable satellite monitoring systems and the central multi-monitor gateway on the local ecosystem (`selfso.com`) using parallel headless Chromium instances.

The evaluation inspected every visible end-user UI component across four dedicated testing tracks:
1. **Satellite 1: NetMon** (`http://192.168.8.30:3884/`) — Network sniffer, SNMP WAN bandwidth telemetry, and LAN device registry.
2. **Satellite 2: MeshMon** (`http://192.168.8.245:16880/`) — Meshtastic LoRa radio gateway, RF spectrum analytics, spatial radar, and live packet decoder.
3. **Satellite 3: EmbDev** (`http://192.168.8.30:3886/`) — Embedded development environment, serial console streaming, out-of-band MCU telemetry, and target lifecycle health.
4. **Gateway Hub: Aimon Multi-Monitor Hub** (`http://127.0.0.1:3883/`) — Centralized multi-monitor navigation, persistent iframe orchestration, offline status overlays, and cross-subsystem routing.

---

## System 1: NetMon (Network & SNMP Telemetry Gateway)
*Target URL: `http://192.168.8.30:3884/` | Host: `rhino`*

### 1.1 UI Component Inspection Matrix

| UI Component | State / Content | Usability Assessment |
|---|---|---|
| **Header Navbar** | `netmon TELEMETRY & SNMP GATEWAY`, `ONLINE` badge, `Uptime: 70h 33m`, `DB: 49.12 MB` | **Excellent**. Clean visual hierarchy, prominent health indicators, and high contrast. |
| **WAN Uplink Telemetry** | Timeframe buttons (`Hour`, `Day`, `Week`, `Month`, `Year`), `SNMP v2c / 64-bit HC` badge | **Very Good**. Timeframe switching is instant; active button state is clear. |
| **Interface Cards (`PPP11` & `PPP12`)** | Inbound/Outbound real-time rate meters (`0.22 Mbps` In, `8.09 Mbps` Out), status `UP (0M)` | **Excellent**. Color-coded meters (Cyan Inbound, Magenta Outbound) align with graph curves. |
| **WAN Throughput Charts (`canvas-ppp11`, `canvas-ppp12`)** | 24-Hour traffic curves, X-axis timestamps (`09:46`, `15:46`, `21:46`, `03:46`, `Now`), Y-axis scales (`11.7M`, `7.8M`, `3.9M`, `0.0M`) | **Very Good**. Far superior to raw SVG charts; includes clear time axes and peak stats (`57.49 M @ 14:13`). |
| **WAN Summary Metrics** | `5M AVG IN`, `5M AVG OUT`, `PEAK IN (24H)`, `TOTAL IN/OUT (24H)` | **Clear & Informative**. Direct insight into rolling averages and total volume (`8.5 / 4.2 GB`). |
| **LAN Throughput Card** | `0.76 Mbps`, Total Packets: `349,671,303`, Capture State: `Active` | **Good**. Immediate visibility into promiscuous PCAP capture engine health. |
| **Protocol Breakdown Card** | Horizontal segmented progress bar (`HTTPS`, `DNS`, `SSH`, `Other`) | **Good**. Color-coded distribution of active network flows. |
| **Top Bandwidth Hosts (15m)** | Displays `"No active flows in window"` when traffic is below sniffer thresholds | **Needs Polish**. Plain text empty state could benefit from an informational icon or threshold indicator. |
| **Discovered LAN Devices Table** | `137 Devices`, Instant search input (`Search IP, MAC, Name, or Vendor...`), Sortable columns (`Device Name`, `IPv4 Address ▲`, `MAC Address`, `OUI Vendor`, `Category`, `Last Seen`) | **High Usability**. Search filter operates instantaneously in memory; category tags (`Infrastructure`, `Workstation`, `IoT`) provide instant context. |

### 1.2 Usability & Functional Deficiencies
1. **Lack of Canvas Hover Tooltips**: Moving the mouse over the canvas traffic graphs does not display an interactive cursor crosshair or tooltip with exact Mbps values at specific points in time.
2. **Table Virtualization Missing for Large Subnets**: Rendering 137 table rows in a single DOM block produces extensive vertical scrolling on mobile/tablet viewports; pagination or infinite scroll would improve UX.
3. **Mobile Layout Margin Shift**: On viewports < 400px, WAN cards exhibit minor horizontal overflow.

---

## System 2: MeshMon (Meshtastic LoRa RF & Telemetry Gateway)
*Target URL: `http://192.168.8.245:16880/` | Host: `fox`*

### 2.1 UI Component Inspection Matrix

| UI Component | State / Content | Usability Assessment |
|---|---|---|
| **Header Navbar** | `meshmon Meshtastic RF & Automation Gateway`, `Radio: gara (!1c160c08)`, `DB: 213,314 pkts`, `Live Stream`, `View Only` lock | **Very Rich**. Immediate visibility into radio hardware connection and database size. |
| **Top KPI Cards (5 Cards)** | `Gateway Node (!1c160c08)`, `Active Fleet (328 Nodes)`, `Airtime & Channel (0.0%)`, `Host Hardware (54.0°C, Uptime 2d 22h)`, `Packet Storage (48.8 MB)` | **Excellent**. Provides comprehensive diagnostic summary across radio, network, host OS, and database. |
| **Sub-Navigation Tabs (9 Tabs)** | `Mesh Network Insights`, `Live Packet Sniffer (0)`, `Spatial Radar`, `Remote Behavior (0)`, `Mesh Topology`, `Fleet Nodes`, `HomeMesh Automation`, `Mesh Chat & AI`, `SQL Console` | **Very High Utility**. Covers complete operator spectrum from RF physics to automation and direct SQL debugging. |
| **Mesh Observability Panel** | `Direct vs Relayed Traffic` progress bar, `Average Propagation Depth`, `Backbone Repeaters`, `Mesh Loop / Storm Watch (CLEAR)` | **Innovative & Clear**. Excellent diagnostic metrics for mesh health and routing loops. |
| **Direct Distance Table** | 295 discovered nodes with distance calculations (e.g. `31.59 km`), coordinates, and altitude | **High Value**. Essential for spatial RF link assessment and antenna alignment. |
| **Live Packet Sniffer** | Real-time packet table, hex/app decoder, portnum filter, text search, Pause/Resume stream | **Highly Functional**. Low-latency diagnostic stream with filtering. |
| **HomeMesh Automation Controls** | `Start Up Pump`, `Open Roof`, `Close Roof`, `Toggle AC`, `Toggle TV` | **Cleanly Integrated**. Guarded with authentication lock. |
| **SQL Console** | Query textarea, Preset buttons (`Recent Packets`, `Telemetry Logs`, `Known Nodes`), Execute button | **High Agency**. Allows power users to run custom analytics queries on local SQLite database. |

### 2.2 Usability & Functional Deficiencies
1. **Initial Empty Card States**: On cold load, several cards on the Insights tab (`RF Hop Count Distribution`, `Meshtastic Protocol Breakdown`) show empty containers or `"Aggregating..."` until background timeseries queries complete.
2. **Sub-Nav Tab Bar Wrapping on Mobile**: 9 navigation buttons wrap into 4 uneven rows on mobile viewports (< 600px), pushing the primary dashboard content far down.
3. **Tab Badge Linebreak Formatting Glitch**: Buttons render as `"Live Packet Sniffer\n 0"` due to unescaped template whitespace, producing uneven tab heights.
4. **Disabled Control Tooltips Missing**: Locked automation buttons (`.btn-auth-guarded`) show a disabled state, but clicking them does not trigger an explanatory tooltip informing the operator to click "View Only" to authenticate.

---

## System 3: EmbDev (Embedded Target Manager & Console Gateway)
*Target URL: `http://192.168.8.30:3886/` | Host: `rhino`*

### 3.1 UI Component Inspection Matrix

| UI Component | State / Content | Usability Assessment |
|---|---|---|
| **Header Navbar** | `embdevenv Gateway Daemon`, `Daemon Connected`, System Clock | **Clean & Direct**. Uncluttered status display. |
| **Target Switcher Tabs** | `📟 n1-655-devkit` (Active), `📟 n1-655-pro` | **Clear**. Fast target selection across multi-board setups. |
| **Power State Card** | `POWER ON` (Glowing green badge), Target metadata (Board model, 3 consoles, MCU telnet port `6073`) | **High Visibility**. High-contrast status indicator. |
| **Hardware Action Buttons** | `▶ Power ON`, `■ Power OFF`, `↻ Reboot`, `🔌 USB Boot`, `⚡ Flash Firmware` | **High Utility**. Complete out-of-band power cycle and recovery controls. |
| **MCU Telemetry Panel** | `CORE TEMP (53.9 °C)`, `FAN SPEED (2111 RPM)`, 30-second temperature sparkline, 12 voltage rail pills (`AVDD08`, `AVDD12`, `DCIN 12.33V`, `SYS_3V3 3.27V`, `SYS_5V 5.05V`, etc.) | **Industry-Grade**. Real-time hardware telemetry prevents board overheating and power rail sagging. |
| **Interactive Serial Terminal** | Serial console dropdown (`console0`), Telnet command shortcut (`telnet 192.168.8.30 6070`), `Freeze`, `Export`, `Expand`, `Clear`, `Auto-scroll`, live xterm.js ANSI terminal | **Outstanding**. Real-time bi-directional streaming terminal allows instant kernel debugging and container inspection without external tools. |
| **Lifecycle & Health Section** | Timeframe buttons (`24h`, `7d`, `30d`, `1y`, `All`), KPI cards (`Firmware Flashes 8`, `Power & Lifecycle 68`, `Intercept & Recovery 43`, `Thermal 53.3°C`) | **Comprehensive**. Long-term tracking of hardware fatigue and reboot reliability. |
| **Wear & Envelope Gauges** | `eMMC Flash Write Stress (0.08%, 8/10,000 cycles)`, `Thermal Operating Envelope (Min 32°C, Max 55.9°C, Ceiling 80°C)` | **Proactive Reliability**. Visual bounds ensure target storage and thermals remain well within safety thresholds. |
| **Lifecycle Events Table** | Timestamped event audit log with filter chips (`All`, `Flashes`, `Reboots`, `Catches`) | **Transparent Audit Trail**. Confirms past recovery and power cycle executions. |

### 3.2 Usability & Functional Deficiencies
1. **Accidental Power-Off Protection**: The `■ Power OFF` and `↻ Reboot` buttons trigger immediate MCU power operations without a secondary confirmation popover (unlike the Flasher modal which has confirmation). Adding a quick 2-second confirmation or hold-to-activate would prevent accidental disruption during debugging.
2. **Terminal Focus Escape Cue**: When clicking inside the interactive terminal, keystrokes are forwarded directly over serial. A subtle visual border highlight or "Press Esc / Click outside to unfocus" label would improve navigation.
3. **Mobile Terminal Line Wrapping**: Terminal width on mobile (375px) forces 80-column kernel output to wrap aggressively.

---

## System 4: Aimon Multi-Monitor Hub (Unified Gateway)
*Target URL: `http://127.0.0.1:3883/` | Host: `builder`*

### 4.1 UI Component Inspection Matrix

| UI Component | State / Content | Usability Assessment |
|---|---|---|
| **Top Navigation Tabs** | `AI Quotas (Self)`, `Agent Telemetry (Analytics)`, `Action Approvals (Mobile)`, `NetMon (netmon)`, `MeshMon (meshmon)`, `EmbDev (embdevenv)` | **Excellent Navigation Structure**. Single-click access across all services with real-time online status dots. |
| **Iframe Embedding Toolbar** | Status dot, Subsystem title, Subsystem badge (`Satellite`), Direct URL, `Reload Tab` button, `Open in New Window` button | **High UX Polish**. Allows operators to reload individual satellite frames or pop them out to dedicated browser windows. |
| **Persistent Iframe Execution** | Hidden/Active panel switching without DOM destruction | **Flawless State Retention**. Switching between tabs keeps live serial sessions (EmbDev) and live packet sniffers (MeshMon) running seamlessly in the background without reloading. |
| **Offline Diagnostic Overlay** | Displays when remote satellite daemon is disconnected: Subsystem name, Host:Port, Last Seen timestamp, and Retry button | **Clear Fault Triage**. Informs user of satellite outages without breaking the rest of the dashboard. |

### 4.2 Usability & Functional Deficiencies
1. **Initial Iframe Loading Skeleton**: Loading a satellite tab across LAN shows a blank dark area until the iframe finishes rendering. A loading spinner or skeleton would improve perceived performance.
2. **Dual Vertical Scrollbars on Laptops**: The combined height of the aimon header, nav tabs, iframe toolbar, and embedded page causes nested scrollbars on laptop screens (1366×768).
3. **URL Hash Synchronization on Cold Start**: Direct navigation to `#netmon` before `/api/monitors` finishes loading briefly displays the default `#aimon` tab before switching.

---

## Consolidated Usability & Action Item Matrix

| Priority | System | Component | Issue Description | Proposed Remediation |
|---|---|---|---|---|
| **P0 (Critical)** | **Aimon** | **Latency Chart** | "Step Duration & Latency Envelope" is confusing, missing color legend indicators, mislabels P95 as Max, and outlier spikes flatten the average line. | 1. Add color swatch badges to legend (`#3b82f6` Average, `#f59e0b` P95 Peak).<br>2. Relabel "Max Duration" to "P95 Envelope (ms)".<br>3. Format Y-axis numbers dynamically (`1.2s`, `45s`, `2.5m`).<br>4. Add time/date labels on the X-axis and hover tooltips. |
| **P1 (High)** | **EmbDev** | **Power Controls** | `■ Power OFF` and `↻ Reboot` execute immediately upon click. | Ensure safety confirmation modal pops up with explicit confirmation buttons before executing power cycle / cut power. |
| **P1 (High)** | **MeshMon** | **Sub-Nav Tabs** | 9-tab navigation wraps into 4 uneven lines on mobile viewports. | Implement horizontal scrolling tab strip with touch swipe on mobile (`overflow-x: auto; white-space: nowrap; -webkit-overflow-scrolling: touch;`). |
| **P1 (High)** | **Aimon Hub** | **Iframe Sizing** | Dual scrollbars appear on laptop resolutions (1366×768). | Calculate `calc(100vh - 150px)` for iframe containers to prevent nested window scrolling. |
| **P2 (Medium)** | **NetMon** | **Traffic Graphs** | Canvas charts lack hover crosshair tooltips with exact Mbps values. | Add mousemove event listener on canvas to display floating tooltip with timestamp and rate. |
| **P2 (Medium)** | **MeshMon** | **Whitespace Glitch** | Tab badges render with unescaped newline (`Live Packet Sniffer\n 0`). | Trim whitespace inside template literals in `web/index.html` and `web/app.js`. |
| **P2 (Medium)** | **NetMon** | **Device Table** | Single 137-row table causes extensive scrolling on mobile. | Add client-side table pagination (25 / 50 / 100 / All per page) with navigation buttons. |
| **P3 (Low)** | **Aimon** | **Favicon** | Missing `/favicon.ico` returns HTTP 404. | Serve inline SVG icon favicon from `WebServer.cxx`. |

---

## Detailed Remediation Plan & Staged Execution Roadmap

### Track 1: Aimon Multi-Monitor Hub & Telemetry Gateway (Local: `builder`)
- **Deliverables**:
  - `web/index.html`: Update `#latency-chart` header with `.dot-blue` and `.dot-amber` badges, rename "Max Duration" to "P95 Envelope", and add latency health threshold subtitle.
  - `web/style.css`:
    - Add horizontal touch scrolling (`overflow-x: auto; white-space: nowrap; -webkit-overflow-scrolling: touch; scrollbar-width: thin;`) to `.monitor-nav`.
    - Set `.monitor-iframe` and `.frame-content-wrapper` height to `calc(100vh - 150px)` to eliminate dual scrollbars on laptop screens.
    - Responsive navbar, Cursor box, and tool progress track styling under `@media (max-width: 600px)`.
  - `web/app.js`:
    - Overhaul `renderSvgLineChart` with dynamic unit scaling (`ms`, `s`, `m`), X-axis timestamp tick marks, and interactive hover crosshairs + tooltips.
    - Implement dual Y-axis scaling for workload velocity (`calls/s` on left, integer sessions on right).
    - Remove dead `#kpi-token-velocity` lookup.
    - Distinguish 0-event active session state in waterfall timeline.
    - Add `title="..."` tooltips on tool matrix rows.
  - `src/WebServer.cxx`:
    - Serve inline SVG favicon on `GET /favicon.ico`.
- **Verification**:
  - `make clean && make -j$(nproc) && make test`
  - Restart daemon in screen session `aimon`.
  - Run `scripts/verify_e2e_ui.mjs` on headless Chromium (1920x1080 and 375x812).

### Track 2: NetMon Bandwidth Charts & LAN Table Pagination (Remote: `rhino` / `/home/samurai/work/netmon`)
- **Deliverables**:
  - `netmon/web/app.js`:
    - Add interactive mousemove / touch canvas crosshair hover inspection on WAN traffic charts (`canvas-ppp11`, `canvas-ppp12`) displaying timestamp and Inbound/Outbound Mbps rates in a floating pill tooltip.
    - Implement client-side pagination for LAN Discovered Devices table (controls for 25 / 50 / 100 / All per page, Previous / Next page buttons, and item counter).
    - Add threshold indicator and icon for empty "Top Bandwidth Hosts (15m)" state.
  - `netmon/web/style.css`:
    - Add pagination controls styling (`.pagination-bar`, `.btn-page`, `.page-info`).
    - Adjust mobile padding on WAN cards for screens < 400px.
- **Verification**:
  - Compile `netmon` and restart inside screen `netmon` on `rhino`:
    ```bash
    ssh -n rhino "cd ~/work/netmon && make -j$(nproc)"
    ssh -n rhino "screen -S netmon -X stuff \$'\003'"
    ssh -n rhino "screen -S netmon -X stuff './build/netmon --daemon\n'"
    ```
  - Headless browser validation against `http://192.168.8.30:3884/`.

### Track 3: MeshMon Mobile Sub-Nav & Tab Formatting (Remote: `fox` / `/home/samurai/work/meshmon`)
- **Deliverables**:
  - `meshmon/web/style.css`:
    - Enable horizontal touch scrolling on `.tabs-nav` and `.tabs-row` (`overflow-x: auto; white-space: nowrap; -webkit-overflow-scrolling: touch; scrollbar-width: thin;`).
  - `meshmon/web/index.html` & `meshmon/web/app.js`:
    - Fix tab badge template whitespace so buttons render as `Live Packet Sniffer (0)` without unescaped newline breaks.
    - Add explanatory tooltip on locked `.btn-auth-guarded` buttons informing operator how to authenticate.
- **Verification**:
  - Compile `meshmon` natively on `fox` and restart inside screen `meshmon`:
    ```bash
    ssh -n fox "cd ~/work/meshmon && make -j$(nproc)"
    ssh -n fox "screen -S meshmon -X stuff \$'\003'"
    ssh -n fox "screen -S meshmon -X stuff './build/aarch64/meshmon\n'"
    ```
  - Headless browser validation against `http://192.168.8.245:16880/`.

### Track 4: EmbDev Hardware Safety & Terminal Usability (Remote: `rhino` / `/home/samurai/work/embdevenv`)
- **Deliverables**:
  - `embdevenv/src/WebServer.cxx`:
    - Ensure power off and reboot actions strictly require confirmation modal confirmation.
    - Add visual border focus cue and "Click outside or press Esc to unfocus terminal" helper.
    - Add horizontal scroll container wrapper for ANSI serial terminal on mobile.
- **Verification**:
  - Compile `embdevenv` on `rhino` and restart service:
    ```bash
    ssh -n rhino "cd ~/work/embdevenv && make -j$(nproc)"
    ```
  - Headless browser validation against `http://192.168.8.30:3886/`.

