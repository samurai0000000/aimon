# aimon: Unified AI Quota & Usage Monitor

`aimon` is a lightweight, high-performance C++17 monitor daemon, command-line utility, and Model Context Protocol (MCP) server for tracking usage quotas, weekly limits, and plan metrics across **Google Antigravity** and **Cursor**.

It is designed specifically for developers who work remotely or off-network and cannot access web-based subscription dashboards due to corporate IP restrictions or single sign-on (SSO) geofences.

---

## Key Features

- **Pure C++17 Architecture**: Zero Python, Node, or heavy runtime dependencies. Compiles to a single fast, self-contained binary.
- **MCP Integration (Model Context Protocol)**: Exposes tools directly over stdio JSON-RPC 2.0 for both Cursor and Antigravity chat agents.
- **Corporate Network Bypass**:
  - Cursor: Reads desktop application tokens locally and connects directly to `api2.cursor.sh` (bypassing web SSO restrictions).
  - Antigravity: Probes the local language server daemon via loopback HTTPS Connect-RPC on `127.0.0.1`.
- **Modern Embedded Web Dashboard**: Built-in HTTP server serving a rich, dark-mode single-page interface with live gauges and countdown timers.
- **Home Assistant (MQTT)**: Built-in MQTT publisher with Home Assistant Auto-Discovery and ready-to-import Lovelace view cards.
- **Privacy & Security First**: All credential access stays strictly local on your machine. No telemetry, third-party relays, or external logging.

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
2. **Cursor Collector**: Reads local desktop authentication tokens from SQLite state storage (`~/.config/Cursor/User/globalStorage/state.vscdb`) and queries Cursor's IDE backend (`https://api2.cursor.sh`).
3. **Delivery Surfaces**:
   - **MCP Server (`aimon mcp`)**: Enables Cursor and Antigravity AI agents to inspect your usage in natural language.
   - **Web Dashboard (`aimon web`)**: Displays visual progress gauges, credit balances, and reset countdown timers in your browser.
   - **Home Assistant (`aimon daemon`)**: Emits MQTT auto-discovery and live state updates to Home Assistant.

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
| `core` | General development, prompt tracking | `check_antigravity_quota`, `check_cursor_usage`, `get_combined_ai_status` (3 tools) |
| `embedded` | Embedded hardware development (`boards`, `embdevenv`) | Core + all `embdevenv_*` tools (19 tools) |
| `network` | Network monitoring & security (`netmon`, `network`) | Core + `firewall_*`, `lan_*`, `snmp_*` tools (19 tools) |
| `mesh` | LoRa wireless mesh networking (`meshmon`) | Core + all `meshmon_*` tools (9 tools) |
| `all` | Full administrative & gateway access | Full unified catalog across all satellites (48+ tools) |

`aimon` detects profiles automatically from workspace folder names during MCP client initialization (e.g. `boards` -> `embedded`), or you can explicitly select a profile via URL query parameter:

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
> *"Check my Antigravity quota and Cursor usage."*

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
│   ├── ConfigManager.hxx          # Configuration manager (~/.config/aimon/config.json)
│   ├── CursorCollector.hxx        # Cursor SQLite & API client
│   ├── DynamicToolRegistry.hxx    # Thread-safe satellite tool registry & routing
│   ├── HistoryStore.hxx           # SQLite3 time-series diff store (~/.config/aimon/history.db)
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
├── scripts/
│   ├── mobile_permission_relay.py # Dual-IDE hook normalizer for Antigravity & Cursor
│   └── test_web_dashboard.py      # Headless Chromium visual validation harness
├── src/
│   ├── AgentTelemetryDb.cxx       # Agent telemetry time-series & downsampling engine
│   ├── AntigravityCollector.cxx   # Antigravity collector implementation
│   ├── ConfigManager.cxx          # Config manager implementation
│   ├── CursorCollector.cxx        # Cursor collector implementation
│   ├── DynamicToolRegistry.cxx    # Dynamic tool registry implementation
│   ├── HistoryStore.cxx           # History & usage diff store implementation
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
│   ├── TestGatewayTimeout.cxx     # Unit tests for TcpGateway timeout execution
│   ├── TestMobileGateway.cxx      # Unit tests for mobile action approval latching
│   └── TestWebServerApi.cxx       # Integration tests for WebServer REST & WS routes
├── third_party/
│   ├── cpp-httplib/               # Git submodule (https://github.com/yhirose/cpp-httplib)
│   └── json/                      # Git submodule (https://github.com/nlohmann/json)
└── web/
    ├── app.js                     # Dashboard dynamics, SVG charts, and tab switching
    ├── index.html                 # Multi-tab dashboard markup (Quotas, Telemetry, Approvals)
    └── style.css                  # Dark-mode glassmorphic styling and charts

---

## License & Copyright

Copyright (C) 2026, Charles Chiou. All rights reserved.
