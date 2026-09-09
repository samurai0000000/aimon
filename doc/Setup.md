# aimon: Setup & Integration Guide

A step-by-step guide to configuring **Google Antigravity IDE** and **Cursor IDE** with `aimon`, accessing the web dashboard, and enabling Model Context Protocol (MCP) tools in both assistants.

---

## Architecture Overview

```
                      ┌──────────────────────────────────────────────┐
                      │              aimon Daemon                    │
                      │  (Runs on your workstation or build server)  │
                      └───────┬──────────────┬──────────────┬────────┘
                              │              │              │
      ┌───────────────────────┘              │              └──────────────────────┐
      ▼                                      ▼                                     ▼
Collector Layer:                     HTTP & MCP Server:                     Home Assistant:
• Antigravity Language Server        • Web Dashboard (port 3883)            • MQTT Auto-Discovery
  (127.0.0.1 Connect-RPC)            • SSE MCP Server (/sse)                  (Lovelace cards)
• Cursor Desktop Backend             • REST API (/api/status)
  (api2.cursor.sh via token)
```

- **Data Collection**: `aimon` talks directly to the Antigravity local language server process and the Cursor backend API (`api2.cursor.sh`).
- **Client Access**: Antigravity and Cursor connect to `aimon` over standard **HTTP / Server-Sent Events (SSE)** at `http://<host>:3883/sse`. No SSH tunnels, custom wrappers, or subprocesses required.

---

## Step 1: Running the `aimon` Daemon

### 1.1 Compile `aimon`
On your build machine or local workstation, compile using the top-level Makefile:
```bash
cd ~/work/aimon
make -j$(nproc)
```
The compiled binary will be located at `./build/aimon`.

### 1.2 Start the Daemon
Run `aimon` in daemon mode:
```bash
./build/aimon
```
You should see:
```text
[WebServer] Dashboard available at http://0.0.0.0:3883
[WebServer] MCP SSE endpoint available at http://0.0.0.0:3883/sse
[aimon] Running in daemon mode (PID: 12345)
[aimon] Press Ctrl+C to stop.
```

The daemon configuration file is saved at:
- **Linux**: `~/.config/aimon/config.json`
- **Windows**: `%USERPROFILE%\.config\aimon\config.json`

---

## Step 2: Cursor IDE Integration

Integrating Cursor involves two quick parts:
1. Providing your Cursor authentication token so `aimon` can fetch your quota.
2. Registering `aimon` as an MCP server so you can ask Cursor about your quotas in chat.

### 2.1 Obtaining Your Cursor Access Token

Because Cursor's web dashboard is often behind corporate Single Sign-On (SSO) or IP restrictions, `aimon` uses your desktop session token to query the modern Cursor quota API (`https://api2.cursor.sh/auth/usage-summary`).

Choose **Method A** (easiest) or **Method B**:

#### Method A: From your Browser (Easiest & Recommended)
1. In your browser, log in to [cursor.com](https://cursor.com) (or [authenticator.cursor.sh](https://authenticator.cursor.sh)).
2. Press <kbd>F12</kbd> (or right-click anywhere and select **Inspect**) to open Developer Tools.
3. Switch to the **Application** tab (Chrome / Edge / Brave) or **Storage** tab (Firefox).
4. Under **Cookies** in the left sidebar, click `https://cursor.com`.
5. Find the cookie named:
   ```text
   WorkosCursorSessionToken
   ```
6. Double-click its **Value** and copy the entire string (it is a JWT token starting with `ey...`).

#### Method B: From the Cursor Desktop SQLite Database
If Cursor is installed on the same machine where `aimon` runs:
- **Linux**: `~/.config/Cursor/User/globalStorage/state.vscdb`
- **Windows**: `%APPDATA%\Cursor\User\globalStorage\state.vscdb`
- **macOS**: `~/Library/Application Support/Cursor/User/globalStorage/state.vscdb`

Run this SQLite query in terminal to print your token:
```bash
sqlite3 ~/.config/Cursor/User/globalStorage/state.vscdb \
  "SELECT value FROM ItemTable WHERE key = 'cursorAuth/accessToken';"
```

---

### 2.2 Configuring the Token in `aimon`

Open `~/.config/aimon/config.json` in your text editor and paste your token under the `"cursor"` section:

```json
{
  "cursor": {
    "access_token": "<your-cursor-access-token>",
    "auto_discover": true
  }
}
```

*(Alternatively, you can set the environment variable: `export CURSOR_ACCESS_TOKEN="<your-cursor-access-token>"` before starting `aimon`.)*

> [!TIP]
> `aimon` reloads the token from `~/.config/aimon/config.json` automatically during polling cycles without requiring a daemon restart.

---

### 2.3 Registering `aimon` as an MCP Server in Cursor

Connect Cursor to `aimon` so you can use tools like `check_cursor_usage` and `get_combined_ai_status`:

#### Option 1: Edit `mcp.json` directly (Fastest)
Open or create your Cursor MCP configuration file:
- **Windows**: `%USERPROFILE%\.cursor\mcp.json` (e.g. `C:\Users\<username>\.cursor\mcp.json`)
- **Linux / macOS**: `~/.cursor/mcp.json`

Add the `aimon` entry:
```json
{
  "mcpServers": {
    "aimon": {
      "url": "http://<aimon-server-ip-or-host>:3883/sse"
    }
  }
}
```
*(If `aimon` runs on the same machine, use `http://127.0.0.1:3883/sse`. If it runs on a remote server, use your server's hostname or IP, e.g. `http://<server-ip-or-hostname>:3883/sse`.)*

#### Option 2: Using the Cursor Settings UI
1. In Cursor, open **Settings** (<kbd>Ctrl</kbd>+<kbd>,</kbd> on Windows/Linux, or <kbd>Cmd</kbd>+<kbd>,</kbd> on macOS).
2. At the top of the settings page, look for the banner:
   > *"Plugins, MCPs, Skills, and Rules have moved to Customize"*
3. Click **Open Customize →** (or select **Customize** in the settings left sidebar).
4. Scroll to the **MCP** section.
5. Click **Add New MCP Server**:
   - **Name**: `aimon`
   - **Type**: `sse`
   - **URL**: `http://<aimon-server-ip-or-host>:3883/sse`
6. Click the **Refresh** button next to MCP. A green status dot will appear showing **3 tools active**:
   - `check_antigravity_quota`
   - `check_cursor_usage`
   - `get_combined_ai_status`

---

## Step 3: Google Antigravity IDE Integration

### 3.1 How Antigravity Metrics Are Collected
Antigravity IDE runs an internal background language server (`language_server_linux_x64` or `language_server_windows_x64.exe`) on loopback `127.0.0.1`.
- When `aimon` runs on the same machine as Antigravity, it **automatically detects** the running language server process, reads its `--csrf_token` argument, and polls usage quotas via HTTPS Connect-RPC.
- If running in a container or specialized environment, you can specify the token in `~/.config/aimon/config.json`:
  ```json
  {
    "antigravity": {
      "auto_discover": true,
      "csrf_token": "<csrf-token-from-process>"
    }
  }
  ```

---

### 3.2 Registering `aimon` as an MCP Server in Antigravity

Antigravity IDE connects to MCP servers over HTTP/SSE using `mcp_config.json`.

#### 1. Open `mcp_config.json`
Open the configuration file for your operating system:
- **Windows**: `%USERPROFILE%\.gemini\config\mcp_config.json` (e.g. `C:\Users\<username>\.gemini\config\mcp_config.json`)
- **Linux / macOS**: `~/.gemini/config/mcp_config.json`

*(In Antigravity IDE, you can open it with <kbd>Ctrl</kbd>+<kbd>O</kbd> and paste the path, or click the **`...`** menu at the top of the chat panel > **MCP Servers** > **Open Configuration**).*

#### 2. Add the `aimon` Server Definition
```json
{
  "mcpServers": {
    "aimon": {
      "serverUrl": "http://<aimon-server-ip-or-host>:3883/sse"
    }
  }
}
```

> [!IMPORTANT]
> Notice the difference in keys between the two IDEs:
> - **Antigravity** uses `"serverUrl"`
> - **Cursor** uses `"url"`

#### 3. Reload Antigravity
1. Press <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>P</kbd> (or <kbd>F1</kbd>) to open the Command Palette.
2. Type and select: `Developer: Reload Window`.
3. Open the Antigravity Chat panel and check the **`...` > MCP Servers** list. `aimon` will display connected with its 3 tools.

---

## Step 4: Embedded Web Dashboard

Open the dashboard in your web browser:
```text
http://<aimon-server-ip-or-host>:3883/
```
*(Example: `http://localhost:3883` or `http://<server-ip-or-hostname>:3883`)*

### Dashboard Features
- **Google Antigravity Card**:
  - Current plan tier (e.g. `Pro`).
  - Remaining Prompt Credits and Flow Credits.
  - Interactive circular gauges for every model (Gemini 3.7 Flash, Claude Sonnet 4.6 Thinking, etc.) with real-time countdown timers to rolling refresh windows.
- **Cursor Card**:
  - Current plan tier (e.g. `Enterprise` / `Pro`).
  - Fast Requests Pool progress bar (`Used / Limit` requests, percentage used, remaining count).
  - Billing cycle reset date and countdown of days remaining.
- **Auto-Refresh**: Polls the daemon every 5 seconds. You can also click the **Refresh** button in the header to trigger an immediate query.

---

## Step 5: Verification & Diagnostics

### 5.1 Querying the REST API
From any terminal or machine on the network:
```bash
# View aggregated JSON status
curl -s http://<aimon-host>:3883/api/status | jq .

# Trigger a manual sync and retrieve updated state
curl -s -X POST http://<aimon-host>:3883/api/refresh | jq .
```

### 5.2 Testing MCP Tools Over HTTP
You can test tool execution directly using `curl`:
```bash
# Test check_cursor_usage
curl -s -X POST http://<aimon-host>:3883/sse \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"check_cursor_usage","arguments":{}}}'

# Test get_combined_ai_status
curl -s -X POST http://<aimon-host>:3883/sse \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"get_combined_ai_status","arguments":{}}}'
```

### 5.3 Testing in Assistant Chat
In either Cursor Chat or Antigravity Chat, send a prompt:
> *"What are my current AI quotas and usage?"*

The assistant will invoke `get_combined_ai_status` and return a clean markdown table summarizing both Antigravity models and Cursor requests.

---

## Step 6: Home Assistant (HA) Integration (Optional)

`aimon` has a built-in MQTT client with automatic Home Assistant MQTT Discovery.

### 6.1 Enable MQTT in `~/.config/aimon/config.json`
```json
{
  "mqtt": {
    "enabled": true,
    "broker": "homeassistant.local",
    "port": 1883,
    "username": "mqtt_user",
    "password": "mqtt_password",
    "topic_prefix": "aimon",
    "discovery_prefix": "homeassistant",
    "retain": true
  }
}
```

### 6.2 View in Home Assistant
1. Ensure the Mosquitto broker integration is active in Home Assistant.
2. Start `aimon` in daemon mode.
3. In Home Assistant, navigate to **Settings > Devices & Services > MQTT**.
4. The **AI Quota Monitor** device will appear automatically with all entities:
   - Antigravity Prompt Credits sensor
   - Antigravity Flow Credits sensor
   - Per-model quota capacity percentages
   - Cursor Fast Requests Used sensor
   - Cursor Fast Requests Limit sensor
   - Cursor Days Remaining sensor
5. Copy pre-built gauge and card templates from [ha/lovelace_cards.yaml](../ha/lovelace_cards.yaml) into your dashboard.

---

## Configuration Reference Cheat Sheet

| Feature | Cursor IDE | Google Antigravity IDE |
| :--- | :--- | :--- |
| **Config File Path (Windows)** | `%USERPROFILE%\.cursor\mcp.json` | `%USERPROFILE%\.gemini\config\mcp_config.json` |
| **Config File Path (Linux)** | `~/.cursor/mcp.json` | `~/.gemini/config/mcp_config.json` |
| **MCP SSE URL Key** | `"url": "http://<host>:3883/sse"` | `"serverUrl": "http://<host>:3883/sse"` |
| **In-App Settings UI** | Cursor Settings > Open Customize → > MCP | Chat Header `...` > MCP Servers |
| **Reload Command** | Click refresh button in Customize > MCP | Command Palette > `Developer: Reload Window` |
| **Authentication Source** | Browser cookie `WorkosCursorSessionToken` or local `state.vscdb` | Auto-discovered `--csrf_token` from language server process |

---

## Troubleshooting & FAQ

#### Q: Cursor shows "Unauthenticated" or "0% used" with `-- / --` requests.
- **Cause**: Either no token is configured, or an expired token was provided.
- **Fix**: Re-check cookie `WorkosCursorSessionToken` on [cursor.com](https://cursor.com) (or query `state.vscdb`), paste it into `~/.config/aimon/config.json` under `"cursor": { "access_token": "..." }`, and hit **Refresh** on the dashboard.

#### Q: Antigravity shows "Offline".
- **Cause**: Antigravity IDE is not open, or the language server process has closed.
- **Fix**: Open Antigravity IDE. `aimon` will detect the process on its next polling interval (or immediately when you click Refresh).

#### Q: The MCP server fails to connect with "session not found".
- **Cause**: Connecting with an outdated or mismatching session ID.
- **Fix**: `aimon` automatically supports Streamable HTTP and auto-initializes new sessions. Simply reload the IDE window (<kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>P</kbd> -> `Developer: Reload Window` in Antigravity, or click Refresh in Cursor Customize).
