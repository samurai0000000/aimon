# Implementation Plan: Dynamic Self-Advertisement, Multi-Host Support, Persistent Iframes & Offline Overlays

## 1. Objectives & Core Architectural Decisions

1. **Subsystem Self-Advertisement (Zero Hardcoding)**:
   - Each satellite monitor (`netmon`, `meshmon`, `embdevenv`, and any future services) advertises its own presentation metadata over the TCP gateway connection in `gateway/register`:
     - `display_name`: human-readable title (e.g., `"Network Monitor"`, `"Mesh Monitor"`, `"Embedded Dev"`)
     - `short_name`: compact badge/tab text (e.g., `"NetMon"`, `"MeshMon"`, `"EmbDev"`)
     - `priority`: numerical sort rank (`10` for netmon, `20` for meshmon, `30` for embdevenv; `0` for aimon)
     - `web_port`: port number for HTTP UI
     - `web_path`: base path (`"/"`)
   - `aimon` consumes whatever metadata the satellite self-advertises; no hardcoded names or ports exist in `aimon`.

2. **Persistent Multi-Iframe DOM Architecture**:
   - Instead of a single shared `<iframe>` that is destroyed and reloaded on every tab switch, maintain persistent frame panels (`#panel-<id>`) in the DOM for each discovered monitor.
   - Switching tabs toggles visibility (`.active` / `.hidden`), eliminating reload latency and preserving interactive state (scroll position, live WebSocket/SSE streaming, filters, graphs).
   - In `renderMonitorTabs()`, orphaned DOM panels whose IDs are no longer present in the discovered monitor list are explicitly pruned to prevent memory leaks.

3. **Disconnection Detection & Sleek Offline Overlay**:
   - `aimon` tracks satellite lifecycle in real time via its persistent TCP connection on port 3885.
   - Retained endpoints are preserved across disconnects with `connected: false`, `reachable: false`, and `lastSeenEpoch: <timestamp>`.
   - When a subsystem disconnects:
     - Tab status indicator turns red (`tab-indicator-offline`).
     - The corresponding iframe content is grayed out and desaturated (`filter: grayscale(0.85) blur(1.5px); opacity: 0.45; pointer-events: none;`).
     - A dark glassmorphic overlay appears on top of the iframe displaying:
       - Warning icon and status notice.
       - Subsystem Offline Notice: `"<name> is Offline"`.
       - Host details and last-seen timestamp (`"Last seen: <time>"`).
       - Explicit "Reload Tab" recovery button.
     - An immediate SSE `tools_changed` event is broadcast to the frontend, updating the dashboard instantly without manual page refresh.
   - When the satellite reconnects, the overlay automatically lifts, graying-out is cleared, and full interactivity is restored.

4. **Stable Deterministic Element IDs with Dynamic Presentation Disambiguation**:
   - **Stable Internal IDs**: To prevent tab ID flapping and DOM panel orphanage when hosts connect or disconnect, internal IDs are permanently deterministic:
     - Self: `"aimon"`
     - Satellites: `mon.subsystem + "-" + sanitize(mon.host) + "-" + std::to_string(mon.port)` (e.g. `"embdevenv-192-168-8-30-3886"`).
   - **Dynamic Presentation Disambiguation**: Applies strictly to presentation labels:
     - If only 1 instance of a subsystem exists: display title is `mon.name` (e.g. `"Embedded Dev"`), tab text is `mon.shortName` (e.g. `"EmbDev"`).
     - If >1 instances of the same subsystem exist: append the host IP to presentation labels (`"Embedded Dev (192.168.8.30)"`, `"EmbDev@192.168.8.30"`).
   - The underlying DOM panel ID never mutates, ensuring active tab state and scroll positions remain unbroken.

5. **Zero-Latency Fast-Path Reachability & Non-Blocking Probing**:
   - **Fast-Path Status Check**: In `getDiscoveredMonitors()`, if `!mon.connected`, the daemon is already dead over its TCP gateway socket. Mark `mon.reachable = false` immediately with zero network I/O (0ms latency).
   - **Non-Blocking Probing**: For connected clients, reachability probes use a short 200ms socket timeout, or rely on cached reachability status, ensuring `GET /api/monitors` responds in < 2ms without stalling HTTP threads or contending for mutexes.

6. **Multi-Host Tool Provider Fallback & Registry Resilience**:
   - `DynamicToolRegistry` tracks registered tools per client ID (`_clientTools[clientId]`).
   - In `unregisterClient(clientId)`, only tools matching `tool.clientId == clientId` are removed from the active tool map.
   - If a surviving client previously registered a tool with the same name, the surviving client's tool definition is preserved/restored.

7. **Origin Resolution & Asset Cache-Busting**:
   - In `getResolvedMonitorUrl(m)`: dynamically substitute `window.location.hostname` whenever `m.host` is `127.0.0.1` or `localhost`. This guarantees that whichever IP/hostname the operator used to reach `aimon` is automatically used to reach co-located daemons.
   - Asset query strings in `web/index.html` bumped to `?v=1.0.6` matching `CMakeLists.txt`, and `WebServer.cxx` serves `Cache-Control: no-cache, must-revalidate` for root static assets.

---

## 2. Protocol & Data Models

### 2.1 JSON-RPC `gateway/register` Parameters
```json
{
  "jsonrpc": "2.0",
  "method": "gateway/register",
  "params": {
    "subsystem": "netmon",
    "name": "netmon",
    "display_name": "Network Monitor",
    "short_name": "NetMon",
    "priority": 10,
    "web_port": 3884,
    "web_path": "/",
    "description": "Local Area Network (LAN) monitor and telemetry engine",
    "tools": [...]
  }
}
```

### 2.2 `DiscoveredMonitor` Model in `aimon` (`include/Models.hxx`)
```cpp
struct DiscoveredMonitor {
    std::string id;             // Stable ID: aimon or <subsystem>-<host>-<port>
    std::string name;           // Presentation title (disambiguated if >1)
    std::string shortName;      // Tab label text (disambiguated if >1)
    std::string subsystem;     // Subsystem slug (netmon, meshmon, embdevenv)
    std::string host;          // Peer IP
    int port = 0;              // HTTP web port
    std::string path = "/";    // HTTP base path
    bool connected = false;    // Gateway TCP socket connected
    bool reachable = false;    // HTTP port reachable
    bool isSelf = false;       // True for aimon
    int priority = 100;        // Sort rank (0=aimon, 10=netmon, 20=meshmon, 30=embdevenv)
    int64_t lastSeenEpoch = 0; // Unix epoch of last disconnect or registration

    nlohmann::json toJson() const;
};
```

---

## 3. Subsystem Advertisements

| Subsystem | Host | Version | `display_name` | `short_name` | `priority` | `web_port` |
|---|---|---|---|---|---|---|
| **`aimon`** | `builder` | `1.0.6` | `"AI Quotas"` | `"AI Quotas"` | `0` | `3883` |
| **`netmon`** | `rhino` | `1.0.2` | `"Network Monitor"` | `"NetMon"` | `10` | `3884` |
| **`meshmon`** | `fox` | `2.1.18` | `"Mesh Monitor"` | `"MeshMon"` | `20` | `16880` |
| **`embdevenv`** | `rhino` | `0.1.5` | `"Embedded Dev"` | `"EmbDev"` | `30` | `3886` |

---

## 4. Implementation Steps by Component

### Component A: `aimon` Core & Gateway
1. **`include/Models.hxx`**:
   - Add `shortName`, `priority`, `connected`, and `lastSeenEpoch` to `DiscoveredMonitor`.
2. **`include/TcpGateway.hxx` & `src/TcpGateway.cxx`**:
   - In `ClientConnection`: add `shortName`, `priority`, `webReachable`, `lastSeenEpoch`.
   - In `handleIncomingJson`: parse `display_name`, `short_name`, `priority`, `web_port`, `web_path`.
   - Update `_retainedMonitors` on registration with `connected = true, reachable = reachable`.
   - In `clientReadLoop`: on disconnect, mark `_retainedMonitors[key].connected = false; reachable = false; lastSeenEpoch = time(nullptr);` and unconditionally invoke `_onToolsChanged()`.
   - In `getDiscoveredMonitors()`:
     - Use fast-path: if `!mon.connected`, immediately set `reachable = false` (0ms).
     - Construct stable ID: `subsystem + "-" + sanitize(host) + "-" + port`.
     - Count subsystem instances: if >1, append host IP to `name` and `shortName`.
     - Sort by `priority` &rarr; `subsystem` &rarr; `host` &rarr; `port`.
3. **`src/DynamicToolRegistry.cxx`**:
   - In `unregisterClient`: only remove tools matching `clientId`.
4. **`src/WebServer.cxx`**:
   - Serve `Cache-Control: no-cache, must-revalidate` for `index.html`, `style.css`, and `app.js`.

### Component B: Web Interface & Offline Overlay
1. **`web/index.html`**:
   - Structure `#dynamic-panels` container for persistent monitor frame panels.
   - Update cache buster query string to `?v=1.0.6`.
2. **`web/style.css`**:
   - Add styles for `.monitor-frame-panel` and `.frame-content-wrapper`.
   - Add styles for `.offline-overlay`, `.offline-card`, `.offline-icon`, `.offline-details`.
   - Add `.iframe-grayed-out` style (`filter: grayscale(0.85) blur(1.5px); opacity: 0.45; pointer-events: none;`).
3. **`web/app.js` & `include/WebAssets.hxx`**:
   - Maintain map of persistent panels in `#dynamic-panels`.
   - Prune orphaned DOM panels whose IDs are no longer in the discovered list.
   - Dynamically create `#panel-${m.id}` on first appearance with toolbar, iframe, and offline overlay.
   - Add "Reload Tab" buttons in both the panel toolbar and within the offline overlay card.
   - Dynamic host resolution in `getResolvedMonitorUrl` substituting `window.location.hostname` when `m.host` is loopback.
   - SSE listener triggers `fetchMonitors()` on `tools_changed` and SSE reconnect.

### Component C: Satellite Monitors
1. **`netmon/src/AimonGatewayClient.cxx`**:
   - Add `display_name: "Network Monitor"`, `short_name: "NetMon"`, `priority: 10` to `gateway/register`.
2. **`embdevenv/src/AimonGatewayClient.cxx`**:
   - Add `display_name: "Embedded Dev"`, `short_name: "EmbDev"`, `priority: 30` to `gateway/register`.
3. **`meshmon/AimonGatewayClient.cxx`**:
   - Add `display_name: "Mesh Monitor"`, `short_name: "MeshMon"`, `priority: 20` to `gateway/register`.

---

## 5. Sequential Build, Deployment & Verification Sequence

1. **Phase 1: Build & Deploy `aimon`**:
   - Compile: `make -j$(nproc)` in `aimon`.
   - Run tests: `make test`.
   - Deploy: Restart `screen -S aimon` on `builder`.
   - Verification: Query `/api/monitors`.
2. **Phase 2: Build & Deploy `netmon`**:
   - Compile: `make -j$(nproc)` in `netmon`.
   - Deploy: Restart `screen -S netmon` on `rhino`.
   - Verification: Verify `NetMon` title, badge, and tool calls.
3. **Phase 3: Build & Deploy `embdevenv`**:
   - Compile: `make -j$(nproc)` in `embdevenv`.
   - Deploy: Restart `screen -S embdevenv` on `rhino`.
   - Verification: Verify `EmbDev` title, badge, and tool calls.
4. **Phase 4: Build & Deploy `meshmon`**:
   - Compile: `ssh -n fox "cd ~/work/meshmon && make -j$(nproc)"`.
   - Deploy: Restart `screen -S meshmon` on `fox`.
   - Verification: Verify `MeshMon` title, badge, and tool calls.
5. **Phase 5: Offline Overlay & Reconnection Verification**:
   - Interrupt `netmon` in `screen -S netmon` on `rhino`.
   - Verify `aimon` registers disconnection immediately, pushes SSE notification.
   - Verify dashboard tab dot turns red, iframe is grayed out, and glassmorphic overlay appears.
   - Relaunch `netmon` on `rhino` and verify automatic recovery: overlay disappears, iframe returns to full color.
6. **Phase 6: Remote Linux Chrome Headless Verification**:
   - Strictly avoid CDP / browser subagents across the port 9222 SSH reverse tunnel.
   - Run native headless Chromium directly on `builder` (without `--virtual-time-budget` to avoid SSE hangs):
     ```bash
     /home/samurai/.cache/ms-playwright/chromium-1200/chrome-linux64/chrome \
       --headless=new --no-sandbox --disable-gpu --window-size=1920,1080 \
       --screenshot=/tmp/final_dashboard.png http://127.0.0.1:3883/
     ```
   - Verify visual rendering across all tabs and confirm absence of errors.

---

## 6. Deep Pitfall Analysis & Mitigations

### 6.1 Initial Operational Pitfalls & Addressed Mitigations
1. **Headless Chrome Infinite Hang on Persistent SSE Stream**:
   - *Cause*: Running with `--virtual-time-budget=N` never idles due to the open `/sse` HTTP stream.
   - *Mitigation*: Strictly omit `--virtual-time-budget`; use standard `--headless=new --screenshot=...` which captures reliably on page `load`.
2. **Synchronous Port Probing Freezing HTTP Responses**:
   - *Cause*: Probing dead hosts blocks for 400ms per host while holding `_retainedMutex`.
   - *Mitigation*: Fast-path check: if `!mon.connected`, mark `mon.reachable = false` immediately (0ms, no I/O).
3. **Tab ID Flapping on Multi-Host Transitions**:
   - *Cause*: Switching `mon.id` from `"subsystem"` to `"subsystem-host-port"` breaks active tab selection and orphans DOM panels.
   - *Mitigation*: Keep internal `mon.id` permanently stable (`subsystem-host-port`), applying disambiguation solely to presentation labels.
4. **Tool Name Collisions Across Multiple Hosts**:
   - *Cause*: Multi-host instances of identical daemons clobber tool registry entries.
   - *Mitigation*: Track tool providers per client and restore surviving definitions upon disconnect.
5. **Stale Browser Caching**:
   - *Cause*: Browsers caching old `app.js` and `style.css`.
   - *Mitigation*: Bump asset query string to `?v=1.0.6` and serve `Cache-Control: no-cache, must-revalidate`.
6. **Child Iframe Stale State Post-Restart**:
   - *Cause*: Internal WebSocket/SSE streams inside child iframes failing to auto-recover.
   - *Mitigation*: Dedicated "Reload Tab" buttons in panel toolbar and on the offline overlay.
7. **Mixed Origin LAN Resolution**:
   - *Cause*: Client browser loading `127.0.0.1` instead of satellite host.
   - *Mitigation*: `getResolvedMonitorUrl` dynamically maps `127.0.0.1` to `window.location.hostname`.

### 6.2 Second-Order Pitfall Analysis of the Mitigated Plan
*(Analysis performed on the newly mitigated architecture)*

1. **Second-Order Risk A: DOM Memory Leak from Transient or Flapping Multi-Host Endpoints**:
   - *Analysis*: If multiple ephemeral satellite instances connect with distinct ports or IPs, `#dynamic-panels` could accumulate DOM nodes.
   - *Safeguard*: `renderMonitorTabs()` computes a set of valid active IDs from `monitors` on each refresh and prunes any DOM panel not present in the set:
     ```javascript
     const activeIds = new Set(list.map(m => `panel-${m.id}`));
     dynamicPanelsEl.querySelectorAll('.monitor-frame-panel').forEach(p => {
         if (!activeIds.has(p.id)) p.remove();
     });
     ```
2. **Second-Order Risk B: Race Condition Between Client Disconnect and Immediate Reconnect**:
   - *Analysis*: If a satellite restarts in < 500ms, its new TCP socket may connect before the old socket's `read()` loop detects EOF.
   - *Safeguard*: `_retainedMonitors` is keyed by `subsystem@peerIp:webPort`. The new connection's `gateway/register` immediately updates `_retainedMonitors[key]` with `connected = true` and the new `clientId`. In `clientReadLoop`, the disconnect handler verifies that the client being cleaned up matches the currently registered client before marking the retained entry offline.
3. **Second-Order Risk C: Tool Execution Deadlock During Gateway Client Disconnect**:
   - *Analysis*: If an in-flight tool call is pending on a client that drops its connection, the calling thread could wait up to the default timeout (45s).
   - *Safeguard*: In `clientReadLoop`, upon detecting socket EOF/disconnect, `TcpGateway` iterates through all pending calls for that client, sets `errorMessage = "Subsystem disconnected during tool execution"`, marks `completed = true`, and calls `cv.notify_all()`, releasing the calling thread instantly (< 1ms).
4. **Second-Order Risk D: Sanitization Collisions in Hostname/IP Strings**:
   - *Analysis*: If dots in IP addresses are replaced with hyphens (e.g. `192-168-8-30`), could an IPv6 or domain name collide?
   - *Safeguard*: Standard IPv4 addresses with port numbers form strictly unique alphanumeric slugs (`"embdevenv-192-168-8-30-3886"`). The slug is valid for HTML IDs and CSS selectors without escaping.

---

## 7. Verification & Implementation Results

- **All 4 Subsystems Integrated**:
  - `aimon`: Self, priority 0, HTTP port 3883.
  - `netmon`: Self-advertised `display_name: "Network Monitor"`, `short_name: "NetMon"`, priority 10, HTTP port 3884.
  - `meshmon`: Self-advertised `display_name: "Mesh Monitor"`, `short_name: "MeshMon"`, priority 20, HTTP port 16880.
  - `embdevenv`: Self-advertised `display_name: "Embedded Dev"`, `short_name: "EmbDev"`, priority 30, HTTP port 3886.
- **Dynamic Self-Advertisement**:
  - Verified zero hardcoded titles, short names, ports, or priorities in `aimon`.
  - `/api/monitors` correctly outputs the dynamically advertised parameters in sorted priority order.
- **Persistent DOM Iframes**:
  - Each monitor tab maintains a dedicated `#panel-<id>` DOM container. Switching tabs activates/deactivates panels without reloading or tearing down iframes.
- **Disconnection Detection & Overlay**:
  - Stopping `netmon` on `rhino` immediately turns its tab indicator red (`#ef4444`), marks `/api/monitors` `connected: false`, applies grayscale/blur desaturation to the iframe, and displays a glassmorphic offline card with last-seen timestamp and reload button.
  - Relaunching `netmon` immediately restores the green indicator, lifts the overlay, and returns the panel to active interactive state.
- **Automated Tests**:
  - `make test` executes `test_gateway_timeout`, verifying formula correctness, 300ms timeout handling, quick success, and clean socket disconnect recovery (100% pass).
- **Headless Chrome Visual Verification**:
  - Verified on native Linux headless Chrome:
    - `dashboard_all_green.png`
    - `dashboard_tab_netmon.png`
    - `dashboard_netmon_offline.png`
    - `dashboard_netmon_recovered.png`
    - `dashboard_tab_meshmon.png`
    - `dashboard_tab_embdev.png`
