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

## Step 6: Agent Telemetry Hooks (Optional)

The **Agent Telemetry** tab (`http://<aimon-host>:3883/#telemetry`) charts sessions, turns, tool calls, error rates, and tool latency. Cursor, Antigravity and Claude Code can feed it through lifecycle hooks that post events to `aimon`.

These hooks are **statistics only**. They never ask for approval, never block a tool, and never wait on a phone or dashboard. If `aimon` is down, the hook drops the event and the agent continues.

> [!WARNING]
> Do not register a telemetry reporter on `preToolUse` (Cursor) or `PreToolUse` (Antigravity), and do not set `failClosed: true`. Those events decide whether a tool may run. A slow or failing reporter there stalls or blocks the agent.

### 6.1 The Reporter Script

Both IDEs can share one small reporter script. Place it anywhere readable, for example:

```text
~/.local/share/aimon/hooks/aimon_telemetry.py
```

The script must:

1. Read the hook payload as JSON from **stdin**.
2. Post one event to `aimon` with a short timeout (under 1 second), ignoring every network error.
3. Print an empty JSON object `{}` to **stdout** and exit `0`. For Antigravity's `Stop` event, print `{"decision": "allow"}` so the agent is allowed to stop.
4. Send only statistics: tool name, duration, status, and counts. Do not forward command lines, file contents, or tool output.

By default the reporter should target `http://127.0.0.1:3883`. Let users override it with an environment variable such as `AIMON_ENDPOINT=http://<aimon-host>:3883`.

### 6.2 The Ingest Endpoint

Hooks post to:

```text
POST http://<aimon-host>:3883/api/telemetry/event
Content-Type: application/json
```

Every event carries `event_type`, `session_id`, `agent_type` (`cursor`, `antigravity` or `claude`), and a Unix `timestamp` in seconds.

**Session start** creates the session row:

```json
{
  "event_type": "SESSION_START",
  "session_id": "<conversation-id>",
  "conversation_id": "<conversation-id>",
  "agent_type": "cursor",
  "workspace": "/path/to/workspace",
  "model": "<model-name>",
  "timestamp": 1790000000
}
```

**Tool call** records one tool execution. `TOOL_CALL` is what the overview and activity charts count, and `tool_name` feeds the tool matrix:

```json
{
  "event_type": "TOOL_CALL",
  "session_id": "<conversation-id>",
  "agent_type": "cursor",
  "tool_name": "Shell",
  "step_index": 12,
  "duration_ms": 420.0,
  "status": "OK",
  "timestamp": 1790000000,
  "details": {
    "workspace": "/path/to/workspace",
    "model": "<model-name>"
  }
}
```

Set `"status": "ERROR"` for a failed tool. `aimon` ignores a repeated event with the same `session_id`, `step_index`, `event_type`, and `tool_name`, so a retried post does not double-count.

**User turn** increments the turn counter:

```json
{
  "event_type": "USER_TURN",
  "session_id": "<conversation-id>",
  "agent_type": "cursor",
  "step_index": 13,
  "status": "OK",
  "timestamp": 1790000000
}
```

**Session end** writes the final totals the reporter has accumulated:

```json
{
  "event_type": "SESSION_END",
  "session_id": "<conversation-id>",
  "agent_type": "cursor",
  "status": "COMPLETED",
  "total_turns": 4,
  "prompt_tokens": 120000,
  "comp_tokens": 8000,
  "tool_calls": 37,
  "errors": 1,
  "avg_turn_ms": 45000.0,
  "timestamp": 1790000000
}
```

`status` may be `RUNNING`, `COMPLETED`, `ABORTED`, or `ERROR`. A reporter can send `SESSION_END` with `RUNNING` after each turn to refresh live totals.

### 6.3 Cursor: `~/.cursor/hooks.json`

Create a user-level hooks file so every workspace reports:

```json
{
  "version": 1,
  "hooks": {
    "sessionStart": [
      { "command": "python3 ~/.local/share/aimon/hooks/aimon_telemetry.py", "timeout": 2, "failClosed": false }
    ],
    "postToolUse": [
      { "command": "python3 ~/.local/share/aimon/hooks/aimon_telemetry.py", "timeout": 2, "failClosed": false }
    ],
    "postToolUseFailure": [
      { "command": "python3 ~/.local/share/aimon/hooks/aimon_telemetry.py", "timeout": 2, "failClosed": false }
    ],
    "stop": [
      { "command": "python3 ~/.local/share/aimon/hooks/aimon_telemetry.py", "timeout": 2, "failClosed": false }
    ],
    "sessionEnd": [
      { "command": "python3 ~/.local/share/aimon/hooks/aimon_telemetry.py", "timeout": 2, "failClosed": false }
    ]
  }
}
```

What each event is used for:

| Cursor event | Reporter action | Useful payload fields |
| :--- | :--- | :--- |
| `sessionStart` | Send `SESSION_START` | `session_id`, `model`, `workspace_roots` |
| `postToolUse` | Send `TOOL_CALL` with `status: OK` | `conversation_id`, `tool_name`, `tool_use_id`, `duration` (ms) |
| `postToolUseFailure` | Send `TOOL_CALL` with `status: ERROR` | same as above |
| `stop` | Send `USER_TURN`, then `SESSION_END` with `RUNNING` | `conversation_id`, `status`, `input_tokens`, `output_tokens` |
| `sessionEnd` | Send final `SESSION_END` | `session_id`, `reason`, `duration_ms` |

Cursor includes `hook_event_name` in every payload, so a single script can branch on it. Use `tool_use_id` to skip duplicate tool events. Tool events can sometimes arrive with an empty `conversation_id`; keep the last known session id in a small state file and fall back to it.

Cursor reloads `~/.cursor/hooks.json` automatically. Check the **Hooks** output channel if a hook does not run.

### 6.4 Antigravity: `~/.gemini/config/hooks.json`

Antigravity reads hooks from a customization root. Use the global root so every workspace reports:

- **Linux / macOS**: `~/.gemini/config/hooks.json`
- **Windows**: `%USERPROFILE%\.gemini\config\hooks.json`

```json
{
  "aimon-telemetry": {
    "PostToolUse": [
      {
        "matcher": "",
        "hooks": [
          {
            "type": "command",
            "command": "python3 ~/.local/share/aimon/hooks/aimon_telemetry.py --event post-tool",
            "timeout": 2
          }
        ]
      }
    ],
    "PreInvocation": [
      {
        "type": "command",
        "command": "python3 ~/.local/share/aimon/hooks/aimon_telemetry.py --event pre-invocation",
        "timeout": 2
      }
    ],
    "Stop": [
      {
        "type": "command",
        "command": "python3 ~/.local/share/aimon/hooks/aimon_telemetry.py --event stop",
        "timeout": 2
      }
    ]
  }
}
```

How this differs from Cursor:

- The top-level key (`aimon-telemetry`) is a hook name. Add `"enabled": false` next to the event lists to turn it off without deleting it.
- `PostToolUse` must be wrapped in a group with a `matcher`. An empty matcher (`""`) matches every tool.
- Antigravity payloads do not include an event name, so pass it on the command line (`--event post-tool`).
- Payload keys are camelCase: `conversationId`, `stepIdx`, `toolCall.name`, `workspacePaths`, `modelName`, and `error` (present only when the tool failed).
- `Stop` must not return `"decision": "continue"`. Return `{"decision": "allow"}` so the agent is free to stop.

`aimon` also scans Antigravity conversation transcripts in the background and assigns them session ids of the form `sess-<first 8 characters of the conversation id>`. Have the reporter use the same form, so hook events and transcript events merge into one session instead of appearing twice.

Reload Antigravity after creating the file (<kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>P</kbd> → `Developer: Reload Window`).

### 6.5 Claude Code: `~/.claude/settings.json`

Claude Code has its own reporter, separate from the Cursor and Antigravity script, because its payloads differ (`session_id`, PascalCase event names such as `PostToolUse`). Place it at:

```text
~/.claude/hooks/aimon_telemetry.py
```

It follows the rules in Step 6.1: statistics only (it never forwards `tool_input`, `tool_response`, prompt text or assistant text), a short HTTP timeout, and it always prints `{}` and exits `0`. It posts `agent_type: "claude"`.

| Claude Code event | aimon event |
| :--- | :--- |
| `SessionStart` | `SESSION_START` (`cwd` becomes the workspace; `model` when Claude Code supplies it) |
| `UserPromptSubmit` | none; marks the start of a turn for latency |
| `PostToolUse` | `TOOL_CALL`, status `OK`, `duration_ms` when supplied |
| `PostToolUseFailure` | `TOOL_CALL`, status `ERROR` |
| `Stop` | `USER_TURN` and a `SESSION_END` with status `RUNNING` (live totals) |
| `SessionEnd` | `SESSION_END`, status `COMPLETED` |

Register it in the user-level settings file so every project reports. User-level hooks merge with a project's own hooks and run alongside them, so a project's `.claude/settings.json` does not need to change:

```json
{
  "hooks": {
    "SessionStart":       [ { "hooks": [ { "type": "command", "command": "python3 \"$HOME/.claude/hooks/aimon_telemetry.py\"", "timeout": 2 } ] } ],
    "UserPromptSubmit":   [ { "hooks": [ { "type": "command", "command": "python3 \"$HOME/.claude/hooks/aimon_telemetry.py\"", "timeout": 2 } ] } ],
    "PostToolUse":        [ { "matcher": "*", "hooks": [ { "type": "command", "command": "python3 \"$HOME/.claude/hooks/aimon_telemetry.py\"", "timeout": 2 } ] } ],
    "PostToolUseFailure": [ { "matcher": "*", "hooks": [ { "type": "command", "command": "python3 \"$HOME/.claude/hooks/aimon_telemetry.py\"", "timeout": 2 } ] } ],
    "Stop":               [ { "hooks": [ { "type": "command", "command": "python3 \"$HOME/.claude/hooks/aimon_telemetry.py\"", "timeout": 2 } ] } ],
    "SessionEnd":         [ { "hooks": [ { "type": "command", "command": "python3 \"$HOME/.claude/hooks/aimon_telemetry.py\"", "timeout": 2 } ] } ]
  }
}
```

Notes:
- Do not register a reporter on `PreToolUse`. Exit code `2` on `Stop` stops Claude from finishing, which is why the reporter never uses it.
- `SessionEnd` has a short default budget (1.5 seconds), so keep an explicit `timeout` on it.
- Claude Code hook payloads carry no token counts. Tokens and estimated cost reach aimon through the Claude usage collector (the Quotas tab and the `check_claude_usage` tool), so the token columns in the Agent Telemetry tab stay `0` for Claude sessions.
- Set `AIMON_ENDPOINT=http://<aimon-host>:3883` to report to another host. Set `AIMON_WORKSPACE=basename` to send only the last path component of the working directory instead of the full path.
- Claude Code loads hooks when a session starts. Start a new session after editing the settings file.

### 6.6 Verifying Telemetry

Send a hand-made tool event from a shell. This mimics a Cursor tool event and should print `{}`:

```bash
printf '%s' '{"hook_event_name":"postToolUse","conversation_id":"telemetry-selfcheck","tool_name":"Shell","tool_use_id":"t1","duration":42}' \
  | python3 ~/.local/share/aimon/hooks/aimon_telemetry.py
```

Then confirm `aimon` stored it:

```bash
curl -s http://<aimon-host>:3883/api/telemetry/tools | jq .
curl -s "http://<aimon-host>:3883/api/telemetry/session_events?sessionId=telemetry-selfcheck" | jq .
```

For Claude Code, send a hand-made event the same way. It should print `{}`:

```bash
printf '%s' '{"hook_event_name":"PostToolUse","session_id":"telemetry-selfcheck-claude","tool_name":"Bash","tool_use_id":"t1","duration_ms":42}' \
  | python3 ~/.claude/hooks/aimon_telemetry.py
curl -s "http://<aimon-host>:3883/api/telemetry/session_events?sessionId=telemetry-selfcheck-claude" | jq .
```

Finally, run one real prompt in each IDE and in Claude Code, and open the **Agent Telemetry** tab. A new session should appear with its tool calls counted. The concurrency chart shows Claude Code as its own amber series.

---

## Step 7: Home Assistant (HA) Integration (Optional)

`aimon` has a built-in MQTT client with automatic Home Assistant MQTT Discovery.

### 7.1 Enable MQTT in `~/.config/aimon/config.json`
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

### 7.2 View in Home Assistant
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
| **Telemetry Hooks File** | `~/.cursor/hooks.json` | `~/.gemini/config/hooks.json` |
| **Telemetry Hook Events** | `sessionStart`, `postToolUse`, `postToolUseFailure`, `stop`, `sessionEnd` | `PostToolUse` (matcher `""`), `PreInvocation`, `Stop` |
| **Telemetry Hooks (Claude Code)** | `~/.claude/settings.json`: `SessionStart`, `UserPromptSubmit`, `PostToolUse`, `PostToolUseFailure`, `Stop`, `SessionEnd` (see Step 6.5) | |
| **Payload Key Style** | snake_case (`conversation_id`, `tool_name`) | camelCase (`conversationId`, `toolCall.name`) |

---

## Troubleshooting & FAQ

#### Q: Cursor shows "Unauthenticated" or "0% used" with `-- / --` requests.
- **Cause**: Either no token is configured, or an expired token was provided.
- **Fix**: Re-check cookie `WorkosCursorSessionToken` on [cursor.com](https://cursor.com) (or query `state.vscdb`), paste it into `~/.config/aimon/config.json` under `"cursor": { "access_token": "..." }`, and hit **Refresh** on the dashboard.

#### Q: Antigravity shows "Offline".
- **Cause**: Antigravity IDE is not open, or the language server process has closed.
- **Fix**: Open Antigravity IDE. `aimon` will detect the process on its next polling interval (or immediately when you click Refresh).

#### Q: The Agent Telemetry tab shows no new sessions.
- **Cause**: The hooks file is not loaded, the reporter cannot reach `aimon`, or the reporter exits with an error.
- **Fix**: Run the self-check in Step 6.6. If it prints `{}` but nothing is stored, check `AIMON_ENDPOINT` and that port 3883 is reachable. For Cursor, open the **Hooks** output channel. For Antigravity, reload the window after editing `hooks.json`. For Claude Code, start a new session after editing `~/.claude/settings.json` and run `/hooks` to confirm the hooks are listed.

#### Q: Tools are waiting on a phone or dashboard approval.
- **Cause**: An approval hook is registered on `preToolUse` / `PreToolUse`, or with `failClosed: true`.
- **Fix**: Remove it. Telemetry hooks belong only on the post-tool, stop, and session events shown in Step 6.

#### Q: The MCP server fails to connect with "session not found".
- **Cause**: Connecting with an outdated or mismatching session ID.
- **Fix**: `aimon` automatically supports Streamable HTTP and auto-initializes new sessions. Simply reload the IDE window (<kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>P</kbd> -> `Developer: Reload Window` in Antigravity, or click Refresh in Cursor Customize).
