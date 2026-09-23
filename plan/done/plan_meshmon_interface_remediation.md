# Architectural Specification & Implementation Plan: MeshMon Interface Remediation & Visual Polish

- **Author**: Charles Chiou
- **Date**: 2026-09-23
- **Status**: Ready for Execution
- **Target Repository**: `meshmon` (`/home/samurai/work/meshmon`) & `aimon` Hub (`/home/samurai/work/aimon`)
- **Target Deployment Host**: `fox` (`192.168.8.245:16880`) & `builder` (`127.0.0.1:3883`)

---

## 1. Executive Summary & Defect Diagnosis

A comprehensive visual and functional inspection across desktop (1920x1080), laptop (1366x768), tablet (768x1024), and mobile (375x812) viewports revealed several critical issues in the **MeshMon** interface:

1. **Severe Mobile & Tablet Breakage (850px Layout Overflow Trap)**:
   - `meshmon/web/style.css` had no mobile media queries (`< 768px`, `< 480px`), forcing `bodyScrollWidth: 850px` on all screens.
   - On tablets (768px) and mobile devices (375px), this forced a massive horizontal scrollbar, clipping the top navbar controls ("DB Size", "Live Stream", "View Only", "Refresh"), KPI ribbons, and tab headers.
2. **Sub-Navigation Tab Bar Clutter & Truncation**:
   - The 9 tabs ("Mesh Network Insights", "Live Packet Sniffer", "Spatial Radar", "Remote Behavior", "Mesh Topology", "Fleet Nodes", "HomeMesh Automation", "Mesh Chat & AI", "SQL Console") were arranged in two uneven fixed rows (`5 + 4`), wrapping awkwardly and truncating labels ("Live Pack[et]", "HomeMesh Automa[tion]").
   - Needs conversion to a responsive, smooth horizontal touch-scrolling single-row bar with clean pill badges.
3. **Unescaped HTML Entity Glitches (`&bull;`)**:
   - In `Packet Storage` top ribbon: `213,314 pkts &bull; 659 nodes` rendered literal `&bull;` due to `.textContent` insertion.
   - In `MeshRoom` card (HomeMesh Automation): `AC Target / Mode: 24°C &bull; cool &bull; auto` rendered literal `&bull;`.
4. **Insights Summary Banner & Analytics Multi-Column Grids**:
   - `.insights-summary-banner` hardcoded `grid-template-columns: 1.5fr 1fr 1fr 1fr;` (4 columns), causing severe text collisions on narrow viewports.
   - `.home-devices-grid` hardcoded `repeat(3, 1fr)` (3 columns), destroying the automation device cards on tablets and phones.
   - `.reach-stats-grid` hardcoded `repeat(3, 1fr)`.
5. **View-Only Mode Guidance**:
   - Clicking automation controls ("Start Up Pump", "Open Roof", "Toggle AC") or transmission buttons in View Only mode should clearly inform the operator to authenticate via the "View Only" navbar button.

---

## 2. Detailed Technical Remediation Plan

### 2.1 Track 1: Responsive Layout & Mobile Breakpoint Suite (`meshmon/web/style.css`)
- **App Container & Viewport Hardening**:
  - Add `overflow-x: hidden; width: 100%;` to `body` and `.app-container`.
- **Top Navbar Responsiveness (`@media (max-width: 768px)`)**:
  - Allow `.navbar` to wrap items cleanly into brand, status chips, and action buttons.
  - Compact status chips (`.status-chip`) on mobile devices.
- **Sub-Navigation Tabs (`.tabs-nav`)**:
  - Refactor into a unified, sleek horizontal touch-scrolling tab strip:
    ```css
    .tabs-nav {
        display: flex;
        flex-direction: row;
        gap: 8px;
        overflow-x: auto;
        white-space: nowrap;
        -webkit-overflow-scrolling: touch;
        scrollbar-width: none;
        padding: 8px 12px;
    }
    .tabs-nav::-webkit-scrollbar {
        display: none;
    }
    .tab-btn {
        flex: 0 0 auto;
        white-space: nowrap;
    }
    ```
- **Top Metrics Ribbon (`.metrics-ribbon`)**:
  - Grid: `grid-template-columns: repeat(auto-fit, minmax(min(100%, 220px), 1fr));`
  - Compact 2x2 grid on mobile/tablet.
- **Insights & Analytics Banners**:
  - `.insights-summary-banner`: Adapt to 2-column or 1-column layout on screens `<= 768px`.
  - `.home-devices-grid`: Adapt from `repeat(3, 1fr)` to `repeat(auto-fit, minmax(min(100%, 320px), 1fr))`.
  - `.reach-stats-grid`: Adapt to 1-column on mobile.
  - `.spatial-layout` / `.analytics-grid`: Stack cleanly into single-column cards on mobile.

### 2.2 Track 2: DOM & Text Entity Cleansing (`meshmon/web/app.js`, `meshmon/web/index.html`)
- Replace literal `&bull;` in `meshmon/web/app.js` with unicode bullet `•`:
  - Line 289: `${data.db.total_packets.toLocaleString()} pkts • ${data.db.total_nodes} nodes`
  - Line 884: `${room.ac_target_temp || 24}°C • ${room.ac_mode || 'cool'} • ${room.ac_fan || 'auto'}`
- Ensure all other entity insertions in index.html and app.js are clean and properly escaped.

### 2.3 Track 3: View-Only Mode Action Feedback (`meshmon/web/app.js`)
- When user clicks any `.btn-auth-guarded` or `.btn-ctrl` in View Only mode, show a clean modal or floating toast:
  *"Click 'View Only' in the top navbar to authenticate with password and unlock controls."*

### 2.4 Track 4: Compilation & Deployment on Fox (`fox`)
- Synchronize `meshmon/include/WebAssets.hxx` fallback strings.
- Compile natively on `fox` via `ssh -n fox "cd ~/work/meshmon && make -j$(nproc)"`.
- Relaunch `./build/aarch64/meshmon` in persistent screen session `meshmon`.

---

## 3. Files to Modify

1. **`meshmon/web/style.css`**: Complete responsive layout overhaul, single-row touch tabs, mobile breakpoints for all 9 dashboard panes.
2. **`meshmon/web/app.js`**: Unicode bullet entity cleansing, touch scrubbing, locked control feedback.
3. **`meshmon/web/index.html`**: Clean semantic tab structure and responsive containers.
4. **`meshmon/include/WebAssets.hxx`**: Synchronize fallback assets.

---

## 4. Verification Plan

1. **Automated E2E CDP Test Suite**:
   - Run `node --experimental-websocket scripts/verify_meshmon_e2e.mjs` covering:
     - Desktop (1920x1080)
     - Laptop (1366x768)
     - Tablet (768x1024)
     - Mobile (375x812)
     - Aimon Hub Embedded View (`http://127.0.0.1:3883/` -> MeshMon tab)
   - Verify `bodyScrollWidth <= window.innerWidth` across all viewports (0 horizontal overflow).
   - Test all 9 sub-navigation tabs.
2. **Compilation & Deployment**:
   - Native build on `fox`: `ssh -n fox "cd ~/work/meshmon && make -j$(nproc)"`.
   - Screen restart on `fox`: session `meshmon`.
