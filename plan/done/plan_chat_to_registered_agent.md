# Horizontal Split Screen (Server Logs Top, Command Outputs Lower), Dedicated 'chat <id>' (Ctrl+D to Exit), and 512-Line History Buffer

## Goal Description

Convert the `aimon` ncurses terminal console to a **horizontal split layout** with a **512-line history buffer** and **Ctrl+D chat exit**:
1. **Header Bar (Top, 1 line)**: Daemon status, active model tiers, connected agent count.
2. **Server Logs (Upper, 5 lines)**: Exactly 5 lines dedicated to original daemon `std::cout` / `std::cerr` logs (e.g. `[WebServer]`, `[TcpGateway]`).
3. **Divider (1 line)**: `--- Command Outputs & Agent Chat [Up/Down/PgUp/PgDn to scroll] ---`.
4. **Command Output & Agent Chat (Lower Panel, "the rest of the screen")**: Dedicated scrollable window for all interactive commands and incoming agent responses, positioned directly above the input prompt.
   - **512-Line History Buffer**: Buffers up to 512 lines of past output and chat.
   - **Keyboard Navigation**: Uses **Up / Down** (line by line) and **PgUp / PgDn** (page by page) to scroll history.
5. **Divider (1 line)**: Horizontal separator above prompt.
6. **Input Prompt (Bottom, 1 line)**: Isolated `aimon> ` or `[#<id> <AgentName>]> ` prompt.
7. **Dedicated `chat <id>` Sub-Shell**:
   - Displays banner: `Press Ctrl+D to exit chat and return to aimon> prompt.`
   - **Zero Keyword Collisions**: The `exit` keyword is **not** used to leave chat mode. Any text containing "exit" (e.g. `exit and plan`) is sent directly to the agent as normal message text.
   - **Ctrl+D (ASCII 4)** is the dedicated key to leave chat mode and return to `aimon> `.
8. **Zero-Token Indefinite Blocking**: `timeout_seconds = 0` in `agent_check_inbox` blocks on kernel `std::condition_variable` without token burn.

---

## Ncurses Terminal Console Layout Specification

### Screen Geometry ($R$ Total Rows $\times$ $C$ Total Columns)

```text
Row 0       ┌─────────────────────────────────────────────────────────────────────────────┐
            │ aimon v1.0.1 │ Antigravity: [ACTIVE] │ Cursor: [PRO: 480 req] │ Agents: 1   │ <- Status Bar (1 line, Blue bg)
Row 1-5     ├─────────────────────────────────────────────────────────────────────────────┤
            │ [WebServer] MCP POST: method=tools/call session='...'                       │
            │ [TcpGateway] Client 192.168.8.245 (meshmon, ID: 1) keep-alive               │ <- Server Logs (stdout/stderr)
            │ [TcpGateway] Client 192.168.8.30 (netmon, ID: 2) keep-alive                 │    (Exactly 5 lines,
            │ [WebServer] SSE client connected, session: 7d7c7674...                      │     auto-scrolls continuously)
            │ [WebServer] MCP GET: /mcp/sse                                               │
Row 6       ├─────────────────────────────────────────────────────────────────────────────┤ <- Divider
            │ --- Command Outputs & Agent Chat [Up/Down/PgUp/PgDn to scroll] [↑ 4 lines] -│
Row 7       │                                                                             │
  ...       │ Connected Agents (1):                                                       │
  ...       │   [1] Antigravity IDE (Host: 127.0.0.1, Session: 7d7c7674...)               │
  ...       │                                                                             │ <- Command Outputs
  ...       │ --- Entering Chat Mode with [#1 Antigravity IDE (127.0.0.1)] ---            │    & Agent Chat
  ...       │ Type messages to send directly.                                             │    (Main workspace:
  ...       │ Press Ctrl+D to exit chat and return to aimon> prompt.                      │     Takes all remaining
  ...       │                                                                             │     vertical height,
  ...       │ [#1 Antigravity IDE (127.0.0.1)] (ref: msg-109):                            │     512-line history
  ...       │ Ready to proceed with the update.                                           │     scrollback buffer)
  ...       │                                                                             │
  ...       │ [You -> #1 Antigravity IDE]:                                                │
Row R-3     │ exit and plan                                                               │
Row R-2     ├─────────────────────────────────────────────────────────────────────────────┤ <- Divider
Row R-1     │ [#1 Antigravity IDE]> _                                                     │ <- Input Line (1 line)
            └─────────────────────────────────────────────────────────────────────────────┘
```

### Coordinate & Pane Breakdown

| Pane Window | Row Coordinates | Height | Contents | Behavior & Controls |
| :--- | :--- | :--- | :--- | :--- |
| **`_headerWin`** | `y = 0` | 1 row | Real-time aggregate status bar | Refreshes daemon state & quota metrics every ~2s |
| **`_logWin`** | `y = 1 .. 5` | 5 rows | Raw daemon `std::cout`/`std::cerr` logs | Background daemon noise (`[WebServer]`, `[TcpGateway]`) auto-scrolls here without clobbering commands |
| **`_midSepWin`** | `y = 6` | 1 row | Section title & scroll indicator | Displays `[↑ <N> lines scrolled]` badge when reviewing history |
| **`_cmdWin`** | `y = 7 .. R-3` | $R - 9$ rows | Command outputs, agent replies, history | **Up / Down**: scroll line by line<br>**PgUp / PgDn**: scroll page by page<br>Buffers up to **512 lines** in memory |
| **`_bottomSepWin`** | `y = R-2` | 1 row | Boundary divider line | Horizontal line (`ACS_HLINE`) separating output from input |
| **`_inputWin`** | `y = R-1` | 1 row | Operator prompt and input line | **Mode 1**: `aimon> `<br>**Mode 2**: `[#<id> <Name>]> `<br>**Ctrl+D**: Exits chat mode back to `aimon> ` |

---

## User Review Required

> [!IMPORTANT]
> ### Chat Mode Exit Behavior
> - In `chat <id>` mode, typing `exit`, `quit`, or `exit and plan` will **not** exit chat mode. It will be sent to the agent as a regular chat message.
> - Pressing **Ctrl+D** is the only way to exit chat mode and return to the `aimon> ` prompt.
> - At the top-level `aimon> ` prompt, `exit` or `quit` (typed alone) or `Ctrl+D` on an empty line will terminate the daemon.

---

## Proposed Changes

### 1. Ncurses Terminal Console (`NcursesConsole`)

#### [MODIFY] [NcursesConsole.hxx](file:///home/samurai/work/aimon/include/NcursesConsole.hxx)
- Update window pointers and geometry:
  - `WINDOW* _headerWin`: Top status bar (1 row at `y = 0`).
  - `WINDOW* _logWin`: 5 lines for `std::cout` / `std::cerr` logs (at `y = 1`).
  - `WINDOW* _midSepWin`: Divider line below server logs (at `y = 6`).
  - `WINDOW* _cmdWin`: Main panel for command outputs and agent chat (at `y = 7`, height: `_termRows - 9`).
  - `WINDOW* _bottomSepWin`: Divider line above input prompt (at `y = _termRows - 2`).
  - `WINDOW* _inputWin`: Bottom row for prompt and input buffer (at `y = _termRows - 1`).
- Add 512-line history buffer and state:
  ```cpp
  static constexpr size_t MAX_HISTORY_LINES = 512;
  struct OutputLine {
      std::string text;
      int colorPair;
      bool isBold;
  };
  std::deque<OutputLine> _cmdHistory;
  int _scrollOffset = 0; // 0 = at bottom (latest), >0 = scrolled up N lines
  ```
- Add chat mode state:
  ```cpp
  int _activeChatAgentId = 0;
  std::string _activeChatSessionId;
  std::string _activeChatClientName;
  ```

#### [MODIFY] [NcursesConsole.cxx](file:///home/samurai/work/aimon/src/NcursesConsole.cxx)
- **Window Setup (`setupWindows`)**:
  - `topRows = 1` (Header, `y = 0`).
  - `logRows = 5` (Server logs, `y = 1`).
  - `sep1 = 1` (Divider with title, `y = 6`).
  - `inputRows = 1` (Prompt, `y = _termRows - 1`).
  - `sep2 = 1` (Divider, `y = _termRows - 2`).
  - `cmdHeight = _termRows - topRows - logRows - sep1 - sep2 - inputRows` (takes rest of screen, `y = 7`).
- **512-Line History Buffer**:
  - `addOutputLine(const std::string& line, int colorPair, bool isBold)`:
    - Appends line to `_cmdHistory`.
    - If `_cmdHistory.size() > MAX_HISTORY_LINES`, pops oldest from front.
    - Clamps `_scrollOffset`.
- **Main Panel Scroll Rendering (`renderMiddlePanel`)**:
  - Draws visible slice of `_cmdHistory` according to `_scrollOffset`.
  - Shows badge `[↑ <N> lines scrolled]` in top-right when scrolled up.
- **Keystroke Handling in `run()`**:
  - `ch == 4` (Ctrl+D):
    - If `_activeChatAgentId > 0`:
      - Leaves chat mode!
      - Sets `_activeChatAgentId = 0`.
      - Prints `[Chat] Exited chat mode. Returned to aimon> prompt.\n\n`.
      - Clears input buffer and calls `redrawInputLine()`.
    - If `_activeChatAgentId == 0` and `_inputBuffer.empty()`:
      - Cleanly triggers daemon shutdown callback.
  - `KEY_UP`: Increments `_scrollOffset` by 1, calls `renderMiddlePanel()`.
  - `KEY_DOWN`: Decrements `_scrollOffset` by 1, calls `renderMiddlePanel()`.
  - `KEY_PPAGE` (Page Up): Increases `_scrollOffset` by `cmdHeight - 2`.
  - `KEY_NPAGE` (Page Down): Decreases `_scrollOffset` by `cmdHeight - 2` (clamps to 0).
- **Command Dispatcher (`processCommand`)**:
  - **In `chat <id>` Mode**:
    - All text lines (including `exit`, `quit`, `exit and plan`) are sent directly to the agent via `AgentMessageBus::postMessageToAgent`. No keywords are intercepted.
    - Resets `_scrollOffset = 0` so new messages are immediately visible.
  - **In Top-Level `aimon>` Mode**:
    - `chat <id>`:
      - Enters chat mode with agent `<id>`.
      - Displays banner:
        ```text
        --- Entering Chat Mode with [#<id> <ClientName> (<IP>)] ---
        Type messages to send directly.
        Press Ctrl+D to exit chat and return to aimon> prompt.
        ```
    - `exit` or `quit`: requires clean zero arguments; shuts down daemon.
    - `agents`, `status`, `clear`, `help`: output to command panel.

---

### 2. Zero-Token Indefinite Blocking

#### [MODIFY] [AgentMessageBus.cxx](file:///home/samurai/work/aimon/src/AgentMessageBus.cxx)
- In `fetchNextMessageForAgent`:
  - If `timeoutSec <= 0`: wait indefinitely on `_cv.wait(lock, hasMessage);`.
  - Wakes up immediately upon `postMessageToAgent` calling `_cv.notify_all()`.

#### [MODIFY] [McpServer.cxx](file:///home/samurai/work/aimon/src/McpServer.cxx)
- Default `timeout_seconds` to `0` in `agent_check_inbox`.

---

### 3. Build & Screen Relaunch

- Native build via top-level `Makefile`:
  ```bash
  make -j$(nproc)
  ```
- Relaunch inside screen session:
  ```bash
  screen -S aimon -X stuff "./build/aimon daemon"$'\n'
  ```

---

## Verification Plan

### Automated / Build Verification
- Compile cleanly:
  ```bash
  make -j$(nproc)
  ```
- Verify zero warnings or linking errors.

### Manual Verification
1. **Layout & Banner Verification**:
   - Inspect layout with `screen -S aimon -X hardcopy /tmp/aimon_screen.txt`.
   - Run `chat 1`.
   - Confirm banner explicitly displays `Press Ctrl+D to exit chat and return to aimon> prompt.`.
2. **No 'exit' Keyword Interception in Chat**:
   - Inside `chat 1`, type `exit and plan`.
   - **Verify**: The text is dispatched to Agent #1. Chat mode does not exit; daemon does not shut down.
   - Type `exit`.
   - **Verify**: The word `exit` is sent to the agent.
3. **Ctrl+D Exit Verification**:
   - Inside `chat 1`, press **Ctrl+D**.
   - **Verify**: Chat mode exits; prompt returns to `aimon> `.
4. **Scroll & 512-Line Buffer Verification**:
   - Use Up/Down and PgUp/PgDn to scroll through up to 512 lines of output.

---

## License & Copyright

Copyright (C) 2026, Charles Chiou. All rights reserved.
