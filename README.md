# aimon: Unified AI Quota & Usage Monitor

`aimon` is a lightweight, high-performance C++17 monitor daemon, command-line utility, and Model Context Protocol (MCP) server for tracking usage quotas, weekly limits, and plan metrics across **Google Antigravity**, **Cursor** and **Claude Code**.

It is designed specifically for developers who work remotely or off-network and cannot access web-based subscription dashboards due to corporate IP restrictions or single sign-on (SSO) geofences.

---

## Key Features

- **Pure C++17 Architecture**: Zero Python, Node, or heavy runtime dependencies. Compiles to a single fast, self-contained binary.
- **MCP Integration (Model Context Protocol)**: Exposes tools directly over stdio JSON-RPC 2.0 for Cursor, Antigravity and Claude Code chat agents.
- **Corporate Network Bypass**:
  - Cursor: Reads desktop application tokens locally and connects directly to `api2.cursor.sh` (bypassing web SSO restrictions).
  - Antigravity: Probes the local language server daemon via loopback HTTPS Connect-RPC on `127.0.0.1`.
  - Claude Code: Reads Claude Code's own local transcripts and reports **estimated** tokens and cost per account, with prices fetched from Anthropic's published pricing page (never embedded).
- **Modern Embedded Web Dashboard**: Built-in HTTP server serving a rich, dark-mode single-page interface with live gauges and countdown timers.
- **Home Assistant (MQTT)**: Built-in MQTT publisher with Home Assistant Auto-Discovery and ready-to-import Lovelace view cards.
- **Agent Telemetry Tab (optional hooks)**: Cursor, Antigravity and Claude Code lifecycle hooks post statistics only (session, turn, tool name, duration, status) to the local daemon. See `doc/Setup.md` Step 6.
- **Usage Insights & Workflow Coaching (`insights.db`)**: Normalizes cross-platform session, token, cache, generation, and numeric prompt-structure metadata across Claude Code, Cursor, and Antigravity (`~/.config/aimon/insights.db`, mode `0600`), runs programmatic and workflow-coaching detectors, exports `sqlite`/`csv`/`jsonl` datasets, and generates weekly digests.
- **Privacy & Security First**: All credential access stays strictly local on your machine. No prompt text, assistant text, thinking text, or raw file paths are ever persisted in `insights.db`.

---

## How It Works

```
                     ┌─────────────────────────────────────────┐
                     │               aimon Core                │
                     │          (In-Memory State Cache)        │
                     └────┬─────────────────┬─────────────┬────┘
                          │                 │             │
        ┌─────────────────┴────┐   ┌────────┴──────┐   ┌──┴────────────────┐
        ▼                      ▼   ▼               ▼   ▼                   ▼
  Antigravity Probe       Cursor Probe         MCP Server       Web Dashboard & MQTT
(Local 127.0.0.1 RPC)  (Local DB -> api2)   (stdio JSON-RPC)     (localhost:3883 / HA)
```

1. **Antigravity Collector**: Automatically scans the local process table for the running `language_server` binary, extracts the launch CSRF token and loopback HTTPS port, and queries the internal `GetUserStatus` Connect-RPC endpoint.
2. **Cursor Collector**: Reads local desktop authentication tokens from SQLite state storage (`~/.config/Cursor/User/globalStorage/state.vscdb`) and queries Cursor's IDE backend (`https://api2.cursor.sh`), including per-category spend (`GetDailySpendByCategory`).
3. **Claude Code Collector**: Reads only the new bytes of each transcript under the Claude config directory (`~/.claude/projects`) on every poll, keeps deduplicated rows and file positions in SQLite, detects the account tier (Enterprise or personal) from the two descriptive strings of the credentials file (no token is ever read into a result), and costs usage in exact integer nano-dollars with a versioned price catalog. Every dollar figure is an estimate and can differ from the billing page. See [Claude Code usage](#claude-code-usage) below.
4. **Usage Insights Engine (`InsightEngine`)**: Incrementally ingests `history.db`, `telemetry.db`, Claude transcripts (`~/.claude/projects`), Antigravity conversation databases and compact transcripts (`~/.gemini/antigravity`), and Cursor checkpoints (`~/.cursor-server`) into `~/.config/aimon/insights.db` to surface programmatic and workflow-coaching findings.
5. **Delivery Surfaces**:
   - **MCP Server (`aimon mcp`)**: Enables Cursor, Antigravity and Claude Code AI agents to inspect quotas and usage insights in natural language (`check_antigravity_quota`, `check_cursor_usage`, `check_claude_usage`, `get_combined_ai_status`, `get_usage_insights`, `get_usage_report`, `export_usage_dataset`, `describe_usage_dataset`, `compare_experiment`).
   - **Web Dashboard (`aimon web`)**: Displays visual progress gauges, credit balances, reset countdown timers, telemetry charts, and the Usage Insights tab in your browser.
   - **Home Assistant (`aimon daemon`)**: Emits MQTT auto-discovery and live state updates to Home Assistant.

### Claude Code usage

Enabled by default; with no configuration it reads `~/.claude` (or `$CLAUDE_CONFIG_DIR`) as one account named `default`. Optional configuration in `~/.config/aimon/config.json`:

```json
"claude": {
  "enabled": true,
  "window_days": 30,
  "retention_days": 400,
  "pricing_refresh_hours": 24,
  "accounts": [
    {"name": "work", "config_dir": "~/.claude", "spend_limit_usd": 500, "cycle_reset_day": 1}
  ]
}
```

- `spend_limit_usd` and `cycle_reset_day` (1-28, UTC) are optional and give "est. $X of $Y" for the current cycle. They are only meaningful for plans with a dollar limit (Enterprise); personal subscription plans show their dollars as API-equivalent value with no budget.
- Two accounts must not share a `config_dir`; a second login on the same machine needs its own Claude config directory.
- Prices come from `https://platform.claude.com/docs/en/about-claude/pricing.md` and are cached next to the configuration. If the page cannot be fetched, the last good prices are used and flagged stale; with none at all, tokens are counted and cost is shown as unpriced.
- Only Claude Code transcripts on the host where the daemon runs are counted (not claude.ai chat, other machines or other products).

### Usage Insights & Workflow Coaching

Enabled by default; writes to `~/.config/aimon/insights.db` (`0600` permissions). Optional configuration in `~/.config/aimon/config.json` (or `aimon.cfg`):

```json
"insights": {
  "enabled": true,
  "db_path": "~/.config/aimon/insights.db",
  "retention_days": 400,
  "rebuild_interval_minutes": 15,
  "cache_efficiency_threshold": 0.50,
  "subagent_share_threshold": 0.35,
  "long_context_threshold_tokens": 150000,
  "compact_context_threshold_tokens": 100000,
  "compact_continue_requests": 20,
  "idle_gap_seconds": 300,
  "error_retry_streak_threshold": 3,
  "burn_rate_alert_fraction": 0.85,
  "short_prompt_max_prose_chars": 60,
  "drip_prompt_max_prose_chars": 80,
  "drip_prompts_per_hour": 8,
  "quick_reprompt_seconds": 120,
  "long_session_hours": 4,
  "coaching_min_sample_size": 10,
  "antigravity_gen_metadata_enabled": true
}
```

CLI subcommands for inspecting insights, generating digests, and exporting datasets:

```bash
./build/aimon insights rebuild [--full]
./build/aimon insights findings [--platform claude|cursor|antigravity] [--days 30] [--min-severity info|warn|crit]
./build/aimon insights report [--platform ...] [--days 30] [--group-by project|model|day|hour_of_day|tool|session]
./build/aimon insights export --dir /path/to/out [--format sqlite|csv|jsonl] [--days 90] [--anonymize]
./build/aimon insights digest [--days 7]
./build/aimon insights compare --detector <name> [--platform ...] [--split-epoch <ts>]
./build/aimon insights describe
```

For comprehensive architectural specifications and protocol details, see [doc/Design.md](doc/Design.md).

---

## Prerequisites

To build `aimon`, ensure the following dependencies are installed on your Linux system:

- **Compiler**: GCC (`g++` 9+) or Clang (`clang++` 10+) with C++17 support.
- **Build System**: CMake 3.16+ and Make (or Ninja).
- **Libraries**:
  - OpenSSL development headers (`libssl-dev`)
  - SQLite3 development headers (`libsqlite3-dev`)
  - POSIX Threads (`pthread`)
- **Git Submodule Dependencies** (managed via `git submodule` under `third_party/`):
  - `third_party/cpp-httplib` (HTTP/HTTPS client and embedded server)
  - `third_party/json` (nlohmann/json C++ modern JSON library)

### Installing Build Dependencies (Debian / Ubuntu)

```bash
sudo apt update
sudo apt install -y build-essential cmake libssl-dev libsqlite3-dev libmosquitto-dev pkg-config
```

---

## Building

```bash
# Clone the repository with submodules (or run git submodule update --init --recursive)
git clone --recurse-submodules <repo-url> aimon
cd aimon

# Compile using the top-level Makefile wrapper
make -j$(nproc)
```

The resulting binary will be created at `./build/aimon`. Clean the build tree anytime with `make clean`.

---

## Usage
 
### 1. Embedded Web Dashboard
Launch the local web server:

```bash
./build/aimon web --port 3883
```

Then open `http://localhost:3883` in your browser. The dashboard automatically refreshes and displays real-time countdown timers toward rolling refreshes and weekly quota caps.

### 2. Full Background Daemon (Web + Home Assistant MQTT)
Run the full background daemon with MQTT publishing to Home Assistant:

```bash
./build/aimon daemon --port 3883
```

This starts:
- The embedded HTTP dashboard on `localhost:3883`.
- The adaptive background poller (5m base / 10m idle).
- The MQTT publisher for Home Assistant auto-discovery.

### 3. Model Context Protocol (MCP) Server (Client-Server SSE)
When `aimon` runs in `daemon` or `web` mode, it provides an official MCP Server-Sent Events (**SSE**) HTTP endpoint on port `3883`. This enables IDE AI assistants across your network to connect directly as HTTP clients without process spawning.

#### Scoped MCP Tool Profiles
To prevent token exhaustion and eliminate irrelevant context in specialized workspaces, `aimon` supports scoped tool profiles:

| Profile | Target Domain | Tools Included |
| :--- | :--- | :--- |
| `core` | General development, quota & insights tracking | Quota (`check_antigravity_quota`, `check_cursor_usage`, `check_claude_usage`, `get_combined_ai_status`) and Insights (`get_usage_insights`, `get_usage_report`, `export_usage_dataset`, `describe_usage_dataset`, `compare_experiment`) tools |
| `eda` / `cad` | PCB design & 3D robotics CAD (`kicad`, `freecad`, `easyeda`, `pcb`, `cad`, `robot`, `hw-`) | Core + all 37 EDA/CAD & CPU accelerator tools (`eda_calc_*`, `cad_calc_*`, `kicad_*`, `freecad_*`, `easyeda_*`) |
| `embedded` | Embedded hardware development (`boards`, `embdevenv`) | Core + all `embdevenv_*` tools |
| `network` | Network monitoring & security (`netmon`, `network`) | Core + `firewall_*`, `lan_*`, `snmp_*` tools |
| `mesh` | LoRa wireless mesh networking (`meshmon`) | Core + all `meshmon_*` tools |
| `mail` | Mail access through `alpine-mcp` | Core + all `mail_*` tools |
| `all` | Full administrative & gateway access | Full unified catalog across all satellites |

`aimon` detects profiles automatically from workspace folder names during MCP client initialization (e.g. `boards` -> `embedded`, `pcb`/`robot`/`cad` -> `eda`), or you can explicitly select a profile via URL query parameter (`?profile=eda` or `?profile=cad`):

#### Configuring in Cursor (SSE)
In Cursor **Settings > Features > MCP > Add New Server** (Type: `sse`):
- **URL**: `http://localhost:3883/sse?profile=embedded` *(or `http://<server-host>:3883/sse?profile=core`)*

Or edit `~/.cursor/mcp.json`:
```json
{
  "mcpServers": {
    "aimon": {
      "url": "http://localhost:3883/sse?profile=core"
    }
  }
}
```

#### Configuring in Antigravity (SSE)
Add `aimon` to your Antigravity MCP server configuration (`mcp_config.json`):
```json
{
  "mcpServers": {
    "aimon": {
      "serverUrl": "http://localhost:3883/sse?profile=embedded"
    }
  }
}
```

*(Note: Stdio transport is also supported for pipe testing via `./build/aimon mcp --profile=core`.)*

Once configured, ask your assistant:
> *"Check my Antigravity quota, Cursor usage, and active usage insights."*

---

## 4. EDA & Robotics CAD Bridge (`EdaBridge`, `EdaAccelerators` & `aimon-eda-bridge`)

`aimon` includes a 100% C++17 AI-assisted hardware design and robotics CAD subsystem exposing **37 MCP tools** under `--profile=eda` / `--profile=cad` (or `--profile=all`):
- **8 Local C++17 CPU Algorithmic Accelerators (`EdaAccelerators`)**: Runs in-process inside `aimon` for sub-100 ms deterministic synthesis and verification (`eda_calc_schematic_layout`, `eda_calc_pcb_placement`, `eda_calc_pcb_autoroute` with 1..6 layer EasyEDA/KiCad layer mapping + PTH pads + `existing_tracks[]`/`existing_vias[]` incremental routing, `eda_calc_pcb_connectivity_audit` for 3D layer-aware Union-Find copper connectivity, broken-net airwire vectors, dangling track endpoints, unassigned pads, and schematic-vs-PCB sync checks, `eda_calc_electrical_synth`, `cad_calc_robot_kinematics_statics`, `cad_calc_mechanism_synth`, `cad_calc_print_orientation_dfm`). Always available even when the remote Windows bridge is offline.
- **11 EasyEDA Pro Tools**: `easyeda_live_status`, `easyeda_search_library`, `easyeda_inspect_schematic` (with live symbol pin coordinates), `easyeda_edit_schematic` (with declarative `connect_pins` pin-to-pin 50-mil orthogonal auto-wiring), `easyeda_sync_sch_to_pcb`, `easyeda_inspect_pcb` (with auto-attached `connectivity_audit`), `easyeda_edit_pcb`, `easyeda_run_drc` (with detailed `check(false, false, true)` violation objects and `connectivity_audit`), `easyeda_export_jlcpcb`, `easyeda_invoke_api`, and `easyeda_convert`.
- **9 KiCad Tools**: `kicad_search_library`, `kicad_inspect_project` (with auto-attached `connectivity_audit`), `kicad_edit_schematic` (with declarative `connect_pins` pin-to-pin 50-mil orthogonal auto-wiring), `kicad_sync_sch_to_pcb`, `kicad_edit_pcb`, `kicad_run_erc`, `kicad_run_drc` (with auto-attached `connectivity_audit`), `kicad_export_fab_pack`, and `kicad_export_step`.
- **9 FreeCAD Robotics & 3D-Printing Tools**: `freecad_live_status`, `freecad_inspect_document`, `freecad_generate_cad`, `freecad_edit_parameters`, `freecad_assemble_joint`, `freecad_analyze_printability`, `freecad_render_view`, `freecad_export_print_files`, and `freecad_invoke_api`.

### Unified Windows `aimon` System Tray Application & Versioned Installer (`make windows-eda-bridge`)

On a Linux workstation, cross-compile the unified Qt6 Windows System Tray application (`aimon.exe`) and package the versioned self-contained Windows installer (`build/windows/aimon-setup-<version>.exe`) with a single non-default `Makefile` target:

```bash
make windows-eda-bridge
# (or alias: make windows-easyeda-bridge)
```

This target automatically fetches official prebuilt Qt 6.8.3 (`win64_mingw`) via `aqtinstall` into `build/qt-prebuilt/`, cross-compiles `aimon.exe` with `x86_64-w64-mingw32-g++-posix`, embeds the companion desktop plugins with zero standalone Python dependencies, and produces:
- `build/windows/aimon-setup-1.1.9.exe` — Self-contained versioned Windows installer & upgrader (`%LOCALAPPDATA%\Programs\aimon\aimon.exe`, Start Menu shortcut, and Windows Installed Apps registration).
- `build/windows/aimon-easyeda-bridge.eext` — Ready-to-import EasyEDA Pro extension archive.

When running on Windows, **`aimon.exe`** docks in the Windows taskbar notification area (`QSystemTrayIcon`) as **`aimon`**:
- **Clicking the `aimon` tray icon** opens the **`aimon`** window on the **AI Quotas** tab, displaying live **Google Antigravity** model/group quota bars & credits, **Cursor** fast requests & cycle spend, and **Claude Code** 5h / 7d / 30d / cycle token & USD spend fetched from `http://<aimon-host>:3883/api/status`.
- **EDA / CAD Bridges & Settings Tab**: Provides a streamlined UI with persisted checkboxes to enable/disable **EasyEDA Pro**, **KiCad**, and **FreeCAD** bridges at runtime, configure the `aimon` Server Host, Web/API Port (`3883`), TCP Gateway Port (`3885`), and Shared Secret, toggle launch at Windows login and auto-deployment of companion plugins, and deploy the embedded KiCad/FreeCAD plugins & EasyEDA `.eext` extension in one click.

### Building `aimon-eda-bridge` on Windows (MSVC)

On a Windows 11 workstation with Visual Studio 2022 C++ Build Tools and CMake:

```powershell
cmake -S tools/eda-bridge -B build/windows-msvc -DAIMON_EDA_BRIDGE_WITH_QT=ON
cmake --build build/windows-msvc --config Release
```

Launch in System Tray GUI mode (default) or headless daemon mode (`--headless`):

```powershell
.\build\windows-msvc\Release\aimon-eda-bridge.exe --listen-host 0.0.0.0 --port 3892 --plugin-port 3893 --secret <shared-secret> --allowed-root Z:\
```

*(Alternatively, pass `--connect <aimon-host>:3885` for reverse inward registration onto `aimon`'s `TcpGateway`.)*

### Installing the Companion Desktop Plugins on Windows

When `Auto-deploy companion plugins on startup` is enabled (the default in `aimon.exe`), or when running `aimon.exe --deploy-plugins "%APPDATA%"`, the KiCad and FreeCAD plugins are deployed automatically:

1. **EasyEDA Pro Extension (`aimon-easyeda-bridge.eext`)**:
   - Exported automatically next to your `aimon` config (`%APPDATA%\aimon\aimon-easyeda-bridge.eext`) or via **Export EasyEDA `.eext` Extension…** in the tray menu. Import in EasyEDA Pro via **Advanced > Extensions > Extension Manager > Import**.
   - Once enabled, the extension connects automatically to `ws://127.0.0.1:3893`.
2. **KiCad `pcbnew` Action Plugin (`%APPDATA%\KiCad\8.0\scripting\plugins\kicad-aimon-bridge` & `9.0`)**:
   - Deployed automatically by `aimon.exe`. In KiCad PCB Editor, click **Tools > External Plugins > Aimon KiCad Live Bridge** to attach to `127.0.0.1:3893`.
3. **FreeCAD Robotics & 3D-Printing Bridge (`%APPDATA%\FreeCAD\Mod\freecad-aimon-bridge`)**:
   - Deployed automatically by `aimon.exe`. FreeCAD loads `InitGui.py` on startup and connects to `127.0.0.1:3893` (when FreeCAD GUI is closed, `aimon` automatically falls back to headless `FreeCADCmd.exe`).

### Configuring in `~/.config/aimon/aimon.cfg` (or Web UI `#aimon/bridges`)

Open `http://localhost:3883/#aimon/bridges` (**tool bridges** tab next to **usage insights**) or add the `eda` block in `~/.config/aimon/aimon.cfg`:

```cfg
eda = {
    enabled = true;
    mode = "bidirectional";
    host = "workstation.local";
    port = 3892;
    plugin_port = 3893;
    linux_path_prefix = "~/work";
    windows_path_prefix = "Z:\\";
    accelerators = { enabled = true; sa_placement_iterations = 50000; autoroute_max_ripup_passes = 25; };
    easyeda      = { enabled = true; execution_mode = "auto"; prefer_jlcpcb_basic_parts = true; };
    kicad        = { enabled = true; execution_mode = "auto"; schematic_grid_mils = 50; };
    freecad      = { enabled = true; execution_mode = "auto"; enforce_tnp_guard = true; };
};
```

---

## Project Structure

```text
aimon/
├── CMakeLists.txt                 # CMake build configuration
├── Makefile                       # Top-level build wrapper (all, clean, distclean)
├── README.md                      # Project overview and quickstart guide
├── doc/
│   ├── Design.md                  # Comprehensive architectural design document
│   └── Setup.md                   # IDE setup & integration guide
├── ha/
│   └── lovelace_cards.yaml        # Ready-to-import Home Assistant Lovelace cards
├── include/
│   ├── AgentTelemetryDb.hxx       # Rolling SQLite WAL agent telemetry & time-series DB
│   ├── AntigravityCollector.hxx   # Antigravity local process probe & Connect-RPC client
│   ├── ConfigManager.hxx          # Configuration manager (~/.config/aimon/aimon.cfg & config.json)
│   ├── CursorCollector.hxx        # Cursor SQLite & API client
│   ├── ClaudeCollector.hxx        # Claude Code transcript collector
│   ├── ClaudeAccount.hxx          # Claude tier detection
│   ├── ClaudeUsageStore.hxx       # Claude usage rows, cursors, price versions (SQLite)
│   ├── PriceCatalog.hxx           # Online price catalog (pricing page parser, cache)
│   ├── DynamicToolRegistry.hxx    # Thread-safe satellite tool registry & routing
│   ├── EasyEdaConverter.hxx       # Native C++17 EasyEDA JSON to KiCad 8 S-expression converter
│   ├── EdaAccelerators.hxx        # 8 native C++17 CPU algorithmic accelerators for EDA & CAD
│   ├── EdaBridge.hxx              # EDA/CAD bridge client, path mapper, and TNP/API guards
│   ├── HistoryStore.hxx           # SQLite3 time-series diff store (~/.config/aimon/history.db)
│   ├── InsightEngine.hxx          # Cross-platform usage insights & workflow coaching engine
│   ├── KiCadSchematicEditor.hxx   # Native C++17 KiCad .kicad_sym/.kicad_sch/.kicad_pcb engine
│   ├── McpServer.hxx              # JSON-RPC 2.0 stdio & SSE engine with scoped profiling
│   ├── MobileGateway.hxx          # Action approval latching & mobile pairing engine
│   ├── Models.hxx                 # Core data structures and metrics
│   ├── MqttPublisher.hxx          # Home Assistant MQTT auto-discovery publisher
│   ├── NcursesConsole.hxx         # Interactive split-screen terminal monitor
│   ├── PathUtils.hxx              # Cross-platform config & database path utilities
│   ├── StateStore.hxx             # Thread-safe in-memory state cache
│   ├── TaskRegistry.hxx           # Client session tracking
│   ├── TcpGateway.hxx             # TCP port 3885 satellite multiplexer
│   ├── WebAssets.hxx              # Embedded fallback dashboard assets
│   └── WebServer.hxx              # Embedded HTTP dashboard server & SSE endpoint
├── src/
│   ├── AgentTelemetryDb.cxx       # Agent telemetry time-series & downsampling engine
│   ├── AntigravityCollector.cxx   # Antigravity collector implementation
│   ├── ConfigManager.cxx          # Config manager implementation
│   ├── CursorCollector.cxx        # Cursor collector implementation
│   ├── ClaudeCollector.cxx        # Claude collector implementation
│   ├── ClaudeAccount.cxx          # Tier detection implementation
│   ├── ClaudeUsageStore.cxx       # Store implementation
│   ├── PriceCatalog.cxx           # Price catalog implementation
│   ├── DynamicToolRegistry.cxx    # Dynamic tool registry implementation
│   ├── EasyEdaConverter.cxx       # EasyEDA JSON to KiCad 8 S-expression converter
│   ├── EdaAccelerators.cxx        # Local C++17 CPU algorithmic solvers for EDA & robotics CAD
│   ├── EdaBridge.cxx              # EDA/CAD bridge client, path translation, and telemetry
│   ├── HistoryStore.cxx           # History & usage diff store implementation
│   ├── InsightEngine.cxx          # Usage insights database, detectors, and dataset export
│   ├── KiCadSchematicEditor.cxx   # Native C++17 KiCad library/schematic/PCB S-expression engine
│   ├── Main.cxx                   # Application entrypoint & subcommand dispatch
│   ├── McpServer.cxx              # MCP tool registration, profiling, and handlers
│   ├── MobileGateway.cxx          # 128-bit mobile pairing, promise latches, & reaper
│   ├── MqttPublisher.cxx          # MQTT auto-discovery and state publisher
│   ├── NcursesConsole.cxx         # Split-screen ncurses console implementation
│   ├── PathUtils.cxx              # Path resolution utilities
│   ├── StateStore.cxx             # State store cache implementation
│   ├── TaskRegistry.cxx           # Session registry implementation
│   ├── TcpGateway.cxx             # TCP satellite gateway multiplexer implementation
│   └── WebServer.cxx              # Dashboard handler, SSE, REST, & telemetry routes
├── test/
│   ├── TestAgentTelemetryDb.cxx   # Unit tests for SQLite WAL rolling timeseries DB
│   ├── TestEasyEdaConverterProps.cxx # RapidCheck property tests for EasyEDA & KiCad 50-mil grid
│   ├── TestEdaBridge.cxx          # Unit tests for EdaBridge path mapper, guards & EdaAccelerators
│   ├── TestEdaBridgeDaemon.cxx    # End-to-end loopback tests for aimon-eda-bridge & all 37 tools
│   ├── TestEdaBridgeProps.cxx     # RapidCheck property tests for EdaBridge & EdaAccelerators
│   ├── TestEdaSurfaces.cxx        # Integration tests for eda MCP profile, /api/eda/* & Web UI
│   ├── TestGatewayTimeout.cxx     # Unit tests for TcpGateway timeout execution
│   ├── TestInsightEngine.cxx      # Unit tests for InsightEngine schema, detectors, and exports
│   ├── TestInsightParserProps.cxx # RapidCheck property & golden tests for wire/prompt parsers
│   ├── TestInsightSurfaces.cxx    # Integration tests for Insights MCP tools & Web endpoints
│   ├── TestLibconfigParser.cxx    # Unit tests for libconfig++ & JSON ConfigManager round-trips
│   ├── TestMobileGateway.cxx      # Unit tests for mobile action approval latching
│   └── TestWebServerApi.cxx       # Integration tests for WebServer REST & WS routes
├── tools/
│   └── eda-bridge/                # Cross-platform C++17 aimon-eda-bridge daemon & GUI plugins
│       ├── CMakeLists.txt         # Standalone Windows MSVC / POSIX CMake configuration
│       ├── include/               # ProcessLauncher, ToolDiscovery, EdaRpcServer headers
│       ├── src/                   # Daemon entrypoint, RPC server, AppPluginRelay (:3893)
│       └── plugins/               # Companion plugins for EasyEDA Pro, KiCad, and FreeCAD
├── third_party/
│   ├── cpp-httplib/               # Git submodule (https://github.com/yhirose/cpp-httplib)
│   └── json/                      # Git submodule (https://github.com/nlohmann/json)
└── web/
    ├── app.js                     # Dashboard dynamics, SVG charts, and tab switching
    ├── index.html                 # Multi-tab dashboard markup (Quotas, Telemetry, Insights, Tool Bridges, Approvals)
    └── style.css                  # Dark-mode glassmorphic styling and charts
```
---

## License & Copyright

Copyright (C) 2026, Charles Chiou. All rights reserved.
