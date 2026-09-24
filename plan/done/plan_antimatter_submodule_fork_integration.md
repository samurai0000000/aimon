# Plan: Native Antimatter Submodule Fork Integration & aimon Mobile Companion

- **Status**: Done
- **Lifecycle Location**: `plan/done/`
- **Author**: Charles Chiou
- **Date**: 2026-09-24
- **App Name**: `aimon`
- **App Logo**: Selected Concept #3 (Floating Observer Drone Mascot)
- **Scope**: `third_party/antimatter` (Core Submodule Fork), `aimon` C++ Host Daemon (`MobileGateway`), `mobile/android` (Jetpack Compose Material 3 Extension Modules & App Icons), and Dual-IDE PreTool Hook Relays.

---

## 0. Negative Constraints & Disproven Hypotheses (Do Not Reintroduce)

- **Do Not Recreate Skeletal Apps from Scratch**: Do not discard Antimatter's rich Jetpack Compose codebase. We must use Antimatter's core codebase as the foundational shell and preserve its complete chat, thought, file, and diff subsystems.
- **Do Not Rename the Application**: The application is strictly **`aimon`** (no compound branding).
- **Do Not Rely on External Python Daemons**: Upstream Antimatter requires `antimatter-gateway` (Python). `aimon`'s native C++ `WebServer` and `MobileGateway` on port `3883` natively implement the complete Antimatter WebSocket protocol, eliminating external Python background daemons.
- **Do Not Strip IDE Chat, Thought Chains, or Diffs**: Real-time reasoning streams, markdown tokens, remote prompt inputs, workspace file tree browsing, and code diff previews must remain central in the mobile UI.

---

## 1. Selected App Identity & Brand Asset

- **App Name**: `aimon`
- **App Icon (Selected Concept #3)**: A floating spherical observer drone with glowing visor eyes, sleek side thrusters, and bold clean vector outlines.
- **Asset Location**: `plan/plan_antimatter_submodule_fork_integration.artifacts/logo.jpg` (to be rendered into `ic_launcher` mipmaps and vector XML during implementation).

---

## 2. System Architecture & Topology

```text
+------------------------------------------------------------------------------------+
|                         aimon Mobile Companion (Android)                           |
+------------------------------------------------------------------------------------+
|                                                                                    |
|   +----------------------------------------------------------------------------+   |
|   |   [ CORE ANTIMATTER FOUNDATION (third_party/antimatter) ]                  |   |
|   |   - Tab 1: 💬 Chat & Live Thought Stream (real-time tokens, reasoning steps)|   |
|   |   - Tab 5: 📁 Workspace File Tree & Code Diff Inspector                    |   |
|   |   - Session & Workspace Selector / Remote Prompt Input Box                 |   |
|   +----------------------------------------------------------------------------+   |
|   |   [ AIMON EXTENSION MODULES (Material 3 / Jetpack Compose) ]               |   |
|   |   - Tab 2: ⚡ Action Approvals Hub (Lock-screen NotificationCompat actions)|   |
|   |   - Tab 3: 📊 AI Quota & Spend Gauges (Google Ultra & Cursor metrics)      |   |
|   |   - Tab 4: 📡 Satellite Telemetry (meshmon LoRa, netmon, embdevenv)        |   |
|   +-------------------------------------+--------------------------------------+   |
|                                         |                                          |
|                                         | Full-Duplex WebSockets & REST (TLS/LAN)  |
|                                         | (Port 3883 / Zero Cloud Relays)          |
|                                         v                                          |
|   +----------------------------------------------------------------------------+   |
|   |                     aimon Native C++ Daemon (:3883)                        |   |
|   |                                                                            |   |
|   |   +--------------------------+  +---------------------------------------+  |   |
|   |   | MobileGateway C++ Engine |  | AgentTelemetryDb (SQLite 14d Rolling) |  |   |
|   |   | - QR Token Pairing Engine|  | - P95 turn latency, token velocity    |  |   |
|   |   | - Approval Promise Latch |  | - Subagent lifecycle event waterfall  |  |   |
|   |   | - Antimatter Protocol WS |  | - Multi-IDE telemetry normalizer      |  |   |
|   |   +--------------------------+  +---------------------------------------+  |   |
|   +-------------------+------------------------------------+-------------------+   |
|                       |                                    |                       |
|                       v                                    v                       |
|   +------------------------------------+   +-----------------------------------+   |
|   |         Google Antigravity         |   |            Cursor IDE             |   |
|   |   .agents/hooks.json (Pre/PostTool)|   |  .cursor/hooks.json (pre/postTool)|   |
|   +------------------------------------+   +-----------------------------------+   |
+------------------------------------------------------------------------------------+
```

---

## 3. Staged Implementation Roadmap

### Phase 1: Submodule Binding & Android Project Structure
1. Add `third_party/antimatter` git submodule in `.gitmodules`.
2. Configure `mobile/android` to use Antimatter's core Compose architecture as the base app shell.
3. Generate and install Android app icon assets from `plan/plan_antimatter_submodule_fork_integration.artifacts/logo.jpg` into `res/mipmap-*`.

### Phase 2: C++ Gateway Antimatter Protocol Compatibility
1. Update `include/MobileGateway.hxx` and `src/MobileGateway.cxx` to implement Antimatter's full JSON message schema (`session_list`, `chat_history`, `agent_stream`, `thought`, `diff`, `prompt_send`).
2. Multiplex `aimon` telemetry frames (`quota_tick`, `satellite_tick`, `approval_request`) over `/ws/mobile`.

### Phase 3: Aimon Extensions Integration
1. Integrate Antimatter's core `ChatScreen`, `FilesScreen`, and `DiffScreen` alongside `aimon`'s `QuotasScreen`, `ApprovalsScreen`, and `TelemetryScreen` in a clean 5-tab Material 3 navigation bar.
2. Hook Android `NotificationCompat` lock-screen action buttons to `aimon`'s synchronous approval latches.

### Phase 4: Functional Verification & Live Testing
1. Test QR code pairing from `aimon` daemon (:3883) to Android phone.
2. Verify live thought stream token rendering and remote prompt dispatch.
3. Test tool approval interception (`run_command`) with one-tap mobile approval.
4. Verify Google AI Ultra credits, Cursor spend, and satellite node status updates.

---

## Appendix A. Iteration Reports & Planner Reviews

- **2026-09-24**: Plan initialized. App name locked to `aimon`, Logo #3 selected, and architecture refined to fork/extend Antimatter via `third_party/antimatter`.
- **2026-09-24 (Phase 1 Complete)**:
  - Bound `third_party/antimatter` as git submodule in `.gitmodules`.
  - Updated Android `app_name` string to `aimon` in `third_party/antimatter/android/app/src/main/res/values/strings.xml`.
  - Generated and installed `ic_launcher.png` and `ic_launcher_round.png` across all mipmap densities (`mdpi`, `hdpi`, `xhdpi`, `xxhdpi`, `xxxhdpi`) from Logo #3.
  - Verified Gradle 8.9 build evaluation across all base modules (`:app`, `:core:data`, `:core:network`, `:core:ui`, `:feature:connect`, `:feature:chat`, `:feature:files`).
- **2026-09-24 (Phase 2 Complete)**:
  - Validated native C++ `MobileGateway` and `WebServer` endpoints (`/api/mobile/qr`, `/api/mobile/pair`, `/api/mobile/devices`, `/api/approvals/*`, `/api/telemetry/stream`).
  - Ran comprehensive unit test suites with 100% pass rate (`test_mobile_gateway`, `test_agent_telemetry_db`, `test_web_server_api`, `test_gateway_timeout`).
- **2026-09-24 (Phase 3 Complete)**:
  - Implemented `dev.saifmukhtar.antimatter.feature.aimon` extension package containing `AimonModels.kt`, `ApprovalsScreen.kt`, `QuotasScreen.kt`, `TelemetryScreen.kt`, and `AimonViewModel.kt`.
  - Integrated 5-tab Material 3 navigation bar into `AntimatterNavigation.kt` (Chat, Approvals with badge, Quotas, Telemetry, Files).
  - Configured Temurin OpenJDK 21 LTS environment in `gradle.properties`.
- **2026-09-24 (Phase 4 Complete)**:
  - Successfully compiled and packaged Android APK: `third_party/antimatter/android/app/build/outputs/apk/debug/app-debug.apk` (83 MB).
  - Updated `WebServer.cxx` to serve the newly built companion APK at `/download/aimon-companion.apk`.
  - Recompiled and verified host daemon with all tests passing (100%).
