# UI Improvements Implementation Plan

## 1. Overview & Goal
This plan tracks requirements, architectural designs, and implementation steps for UI enhancements across the web dashboard and the Android Mobile Companion app (`aimon/mobile/android/`), including a critical fix for the quota countdown timer clock-skew anomaly (`80131h 52m`).

> [!IMPORTANT]
> **Status**: Review Mode. Proposed Antimatter cybernetic overhaul for the Android Companion App and Clock-Skew Countdown Bug Fix. Awaiting user review and approval before execution.

---

## 2. Web Dashboard UI Enhancements (Completed)

- [x] **Top Header Bar Removal (`.navbar`)**: Removed deprecated header bar.
- [x] **Top Monitor Tab Labels (`#monitor-tabs`)**: All lowercase (`aimon`, `netmon`, `meshmon`, `embdevenv`).
- [x] **Viewport & Scrollbar Fix**: Eliminated nested scrollbars on satellite iframe panels.
- [x] **Mobile Companion APK Download Link**: Direct download button in Approvals card.
- [x] **Telemetry Chart Legends**: Color swatches matching SVG curves.
- [x] **Telemetry Sessions Dropdown Formatting & Sorting**: `YYYY/MM/DD HH:MM:SS` sorted newest first.
- [x] **Agent Telemetry 2×2 Charts Grid**: 4 live SVG charts (Workload Velocity, Latency Envelope, Gantt Timeline, Busyness Stack).

---

## 3. Critical Bug Fix: Quota Reset Timer Clock-Skew Anomaly (`80131h 52m`)

### 3.1. Root Cause Analysis
- When calculating remaining quota reset duration in `web/app.js` (`new Date(bucket.reset_time_iso).getTime() - Date.now()`), the client's local wall clock was directly compared against the server's absolute ISO timestamp.
- Any clock disparity (e.g. client device in current calendar year vs server date, or uncalibrated timezones) produced multi-year differences (e.g. `80131h 52m`).
- Additionally, `QuotaBucket` in C++ did not compute server-side `reset_time_remaining_seconds` relative to the daemon's internal clock.

### 3.2. Remediation Plan
1. **Server-Side Relative Duration (`Models.hxx`, `AntigravityCollector.cxx`)**:
   - `QuotaBucket` parses `resetTimestamp` via `parseIsoTimestamp()`.
   - `QuotaBucket::toJson()` and `ModelQuota::toJson()` compute `reset_time_remaining_seconds` using `std::chrono::system_clock::now()`.
   - `AggregateStatus::toJson()` exposes `server_timestamp_ms` and `server_time_iso`.
2. **Client-Side Offset Calibration & Sanity Bound (`web/app.js`, `WebAssets.hxx`)**:
   - Client records `serverTimeOffset = data.server_timestamp_ms - Date.now()`.
   - `updateCountdowns()` calculates countdown using elapsed milliseconds since fetch and `bucket.reset_time_remaining_seconds`.
   - **Sanity Clamping**: If computed duration exceeds weekly window (> 7 days / 604,800s), fallback immediately to canonical duration from `bucket.description` (e.g. `"3 days, 8 hours"` -> `"3d 8h"`).
   - If `bucket.remaining_fraction >= 1.0` or duration `<= 0`, render `"Active"` / `"Fully refreshed"`.
3. **Android Data Layer (`AimonRepository.kt`, `QuotasScreen.kt`)**:
   - Ingest `reset_time_remaining_seconds` and apply identical server-clock offset compensation and sanity clamping.

---

## 4. Android Companion App Antimatter Cybernetic Overhaul (Proposed)

The current Android Compose UI utilizes standard/vanilla Material3 containers. We will transform the mobile companion into a high-tech "Antimatter" cybernetic dashboard matching the futuristic aesthetic of the desktop web app.

### 3.1. Design System & Cybernetic Tokens (`Theme.kt`)
- **Deep Space Void Background**: `0xFF060813` base with radial glow and subtle quantum starfield / particle mesh background canvas.
- **Glassmorphic Antimatter Cards**: Translucent dark surfaces (`0xDD0D1527`) with multi-color neon gradient borders (`Cyan -> Purple -> Indigo`) and tactile depth.
- **Pulsing Quantum Indicators**: Multi-ring animated breathing status dots for live daemon connection and SSE stream.
- **Neon Progress Gauges**: Glowing gradient fill progress meters with luminous head indicators and radial arc capacity gauges.
- **Cyber Monospace Badges**: Neon `QuantumChip` components with custom border glow for model names, quotas, and tool tags.

### 3.2. Quantum App Navigation & Header (`AimonApp.kt`)
- **Cybernetic App Bar**: High-tech header with glowing quantum reactor icon, live latency badge (e.g. `14ms`), and connection state.
- **Antimatter Navigation Bar**: Glowing bottom navigation bar with active route neon indicators and live notification badge pills.

### 3.3. Google Antigravity & AI Quotas Dashboard (`QuotasScreen.kt`)
- **Antigravity Power Card**:
  - Plan tier badge (`Google AI Ultra` / `Pro`) with neon glow.
  - Large monospace Google AI Credits readout with gradient highlight.
  - Dual-meter progress bars for 5-Hour and Weekly limits with color-coded safety thresholds (Cyan -> Emerald -> Amber -> Red).
  - Live reset countdown badges (e.g. `⏱️ 2h 45m`).
  - Collapsible Model Capacities drawer with mini neon progress meters for Gemini, Claude, and GPT models.
- **Cursor Quota & Spend Card**:
  - Fast Requests Pool meter with real-time ratio and percentage.
  - Cumulative Spend card with billing cycle countdown and category spend progress bars.

### 3.4. Action Approvals Tactical Command Console (`ApprovalsScreen.kt`)
- **Radar Zero-State**: Animated cybernetic circular radar scanner sweeping for incoming approval requests.
- **Tactile Approval Card**:
  - Monospace dark code terminal box for tool invocation arguments with syntax-highlighted keys and values.
  - Real-time countdown timer bar with emergency pulsing crimson glow when `<15s` remain.
  - Glowing **Approve** (Emerald Neon) and **Deny** (Crimson Neon) action buttons with tactile ripple feedback.

### 3.5. Live Telemetry & Visual Waveform Charts (`TelemetryScreen.kt`)
- **KPI Matrix**: 4 glassmorphic metric cards (Active Sessions, Total Tool Calls, Avg Step Latency, Tool Error Rate).
- **Custom Canvas Charts**:
  - **Tool Invocations Velocity Waveform**: Real-time smooth curve chart with gradient area fill.
  - **Step Latency Envelope Chart**: P95 latency envelope vs average step duration.
- **System Gateway Health Matrix**: Status badges for aimon Core Daemon, Mobile SSE Stream, and Agent Interceptors.

### 3.6. Satellite Node Telemetry Cards (Netmon, Meshmon, EmbDevEnv)
- **Netmon Card**: Real-time LAN traffic bandwidth, active firewall sessions, top talkers.
- **Meshmon Card**: LoRa radio node status, RF channel utilization, packet count.
- **EmbDevEnv Card**: Target hardware boards (`n1-655-pro`, `n1-655-devkit`), power state, serial console health.

### 3.7. Pairing Station & Radar Scanner (`PairingScreen.kt`)
- **Radar Scan Visual**: High-tech rotating radar reticle.
- **LAN Discovery & Hinting**: Quick-fill server IP suggestions from local network.
- **Pairing Key Formatter**: Monospace format with 128-bit hex validation badge.
- **One-Tap Unpair & Connection Diagnostic Test**.

---

## 4. Implementation Steps & File Mapping

1. **`Theme.kt`**: Add `QuantumCanvasBackground`, `AntimatterArcGauge`, `NeonGlowCard`, and enhanced button styles.
2. **`AimonApp.kt`**: Overhaul top app bar, bottom navigation bar, and background wrapper.
3. **`QuotasScreen.kt`**: Implement full Antimatter cards, dual-meters, credits display, and model capacity drawers.
4. **`ApprovalsScreen.kt`**: Implement animated radar zero-state, code terminal argument viewers, and glowing action buttons.
5. **`TelemetryScreen.kt`**: Implement custom Canvas sparkline/waveform charts and satellite node summaries.
6. **`PairingScreen.kt`**: Implement radar pairing frame and diagnostic controls.
7. **Compilation & Testing**:
   - Run JVM unit tests (`./gradlew test`) validating all parsers and models.
   - Build Android APK (`./gradlew assembleDebug`) and deploy to daemon download path.
   - Verify daemon download endpoint (`/download/aimon-companion.apk`).

---

## 5. Verification Plan

### Automated Tests
- `JAVA_HOME=$HOME/.jdks/jdk-21.0.4+7 ANDROID_HOME=$HOME/Android/Sdk ./gradlew test` (Verify 100% test pass rate).
- `JAVA_HOME=$HOME/.jdks/jdk-21.0.4+7 ANDROID_HOME=$HOME/Android/Sdk ./gradlew assembleDebug` (Verify clean build with zero compile errors).

### Live Verification
- Test APK binary presence: `ls -la mobile/android/app/build/outputs/apk/debug/app-debug.apk`.
- Test daemon HTTP download endpoint: `curl -I http://127.0.0.1:3883/download/aimon-companion.apk`.

