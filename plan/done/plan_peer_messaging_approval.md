# Inter-Agent Conversation Management with Initial Session Approval, Operator Termination, and Dual-Surface Monitoring

## Goal Description

Enable autonomous, cross-network communication between AI agents (specifically Google Antigravity on `127.0.0.1` and Cursor Grok on `192.168.8.216`) routed through `aimon` over MCP/SSE, with **Session-Level Operator Authorization and Control**:
1. **Initial Session Approval**: The operator approves only the *start* of the inter-agent dialogue session. Individual messages within an approved session flow freely without requiring per-message manual approvals.
2. **Operator Termination in `aimon`**: The operator can terminate an active inter-agent conversation at any time from the `aimon` CLI console using `terminate [id]` (or `end [id]`), instantly closing the channel.
3. **Dual-Surface Real-Time Monitoring**:
   - **CLI Console (`NcursesConsole`)**: All conversation messages stream live in the middle scrollable panel with timestamps, session ID, and color-coded sender tags.
   - **Web Dashboard (`http://builder:3883/`)**: A dedicated "Inter-Agent Dialogue" card renders the conversation history in real time with provider-specific glassmorphic bubbles, live active/terminated status indicators, message counts, and duration tracking.

---

## User Review Required

> [!IMPORTANT]
> ### 1. Session Lifecycle Workflow
>
> 1. **Initial Conversation Request**:
>    - Antigravity calls `agent_send_message(target="Cursor Grok", message="Can we coordinate on amba-virt server plan?")`.
>    - Because no active conversation session exists between Antigravity and Cursor Grok, a new session (e.g. `convo-1`) is created in state `PENDING_APPROVAL`.
>    - `aimon` displays the approval banner in the console:
>      ```text
>      ┌─── [CONVERSATION REQUEST: convo-1] ──────────────────────────────────────┐
>      │ Initiator: Antigravity IDE (Gemini)                                      │
>      │ Target   : Cursor Grok (amba-virt)                                       │
>      │ Topic/Msg: "Can we coordinate on amba-virt server plan?"                 │
>      │ Action   : Type 'approve 1' to authorize session, or 'reject 1'.         │
>      └──────────────────────────────────────────────────────────────────────────┘
>      ```
>
> 2. **Session Approval & Autonomous Message Flow**:
>    - Operator types **`approve 1`** (or bare `approve` if only one request is pending).
>    - Session `convo-1` transitions to **`ACTIVE`**.
>    - The initial message is delivered to Cursor Grok's inbox.
>    - **From this point forward, all subsequent messages and replies between Antigravity and Cursor Grok flow autonomously without manual approval prompts.**
>
> 3. **Operator Termination in `aimon`**:
>    - At any point during the conversation, the operator can type:
>      **`terminate 1`** (or **`terminate`** / **`end`** if 1 active session).
>    - Session transitions to **`TERMINATED`**.
>    - The channel is closed immediately. Any pending waits are unblocked and informed that the session was terminated by the operator.
>    - Both CLI and Web indicate the session is terminated. No further messages are routed on that session.

---

## Proposed Changes

### Component 1: Conversation & Message Bus (`AgentMessageBus`)

#### [MODIFY] [AgentMessageBus.hxx](file:///home/samurai/work/aimon/include/AgentMessageBus.hxx) & [AgentMessageBus.cxx](file:///home/samurai/work/aimon/src/AgentMessageBus.cxx)
- **Data Structures**:
  ```cpp
  struct ConversationRecord {
      std::string id;               // e.g. "msg-1"
      std::string convoId;          // e.g. "convo-1"
      int64_t timestampEpoch = 0;
      std::string timestampIso;
      std::string sender;           // e.g. "Antigravity IDE"
      std::string senderSessionId;
      std::string target;           // e.g. "Cursor Grok"
      std::string targetSessionId;
      std::string type;             // "initial_request", "message", "reply"
      std::string message;

      nlohmann::json toJson() const;
  };

  struct AgentConversation {
      std::string id;               // e.g. "convo-1"
      std::string initiator;        // e.g. "Antigravity IDE"
      std::string initiatorSessionId;
      std::string target;           // e.g. "Cursor Grok"
      std::string targetSessionId;
      std::string initialTopic;
      int64_t startTimeEpoch = 0;
      int64_t endTimeEpoch = 0;
      enum State { PENDING_APPROVAL, ACTIVE, REJECTED, TERMINATED } state = PENDING_APPROVAL;
      std::string terminationReason;
      std::vector<ConversationRecord> messages;

      nlohmann::json toJson(bool includeMessages = true) const;
  };
  ```

- **Session Management Methods**:
  - `std::shared_ptr<AgentConversation> getActiveConversation(const std::string& agentA, const std::string& agentB)`:
    - Finds active conversation between the two peers if one exists.
  - `std::shared_ptr<AgentConversation> requestConversation(sender, senderSession, target, initialText)`:
    - Creates `AgentConversation` in `PENDING_APPROVAL` state.
    - Fires notification callback to `NcursesConsole` to display the approval alert banner.
  - `bool approveConversation(const std::string& convoId)`:
    - Moves state from `PENDING_APPROVAL` to `ACTIVE`.
    - Delivers initial message into target's inbox.
    - Wakes up initiator thread waiting on `waitForSessionApproval`.
    - Notifies CLI and Web observers that session is now `ACTIVE`.
  - `bool rejectConversation(const std::string& convoId, const std::string& reason)`:
    - Sets state `REJECTED`, unblocks initiator.
  - `bool terminateConversation(const std::string& convoId, const std::string& reason = "Closed by operator")`:
    - Sets state `TERMINATED`.
    - Unblocks any waiting message loops with termination status.
    - Notifies CLI and Web observers.
  - `std::vector<std::shared_ptr<AgentConversation>> listConversations(bool onlyActive = false)`:
    - For CLI and Web status queries.
  - `std::vector<ConversationRecord> getConversationMessages(const std::string& convoId = "", size_t limit = 100)`:
    - Returns chronological message history for live display.

---

### Component 2: MCP Server & Web Server API

#### [MODIFY] [McpServer.hxx](file:///home/samurai/work/aimon/include/McpServer.hxx) & [McpServer.cxx](file:///home/samurai/work/aimon/src/McpServer.cxx)
- Register tool: **`agent_send_message`**:
  - Parameters:
    - `message` (string, required): Message text.
    - `target` (string, optional, default "Cursor Grok"): Target peer name.
    - `timeout_seconds` (integer, optional, default 60): Timeout waiting for approval on initial start.
  - Handler logic:
    - Checks for existing `ACTIVE` conversation with target.
    - If **None exists**:
      - Initiates session request via `requestConversation`.
      - Waits for operator approval.
      - If approved, delivers initial message and returns `{"status": "session_started", "conversation_id": "convo-1", "message_id": "msg-1"}`.
      - If rejected, returns `{"status": "rejected", "reason": ...}`.
    - If **Active session exists**:
      - Directly routes message to peer inbox without prompting operator!
      - Logs message to conversation history.
      - Returns `{"status": "delivered", "conversation_id": "convo-1", "message_id": "msg-X"}`.
    - If session is **Terminated**:
      - Returns error `{"status": "error", "error": "Conversation was terminated by operator."}`.

- Update **`agent_send_reply`**:
  - Appends reply to the active conversation history and delivers to recipient.
  - If conversation has been terminated, notifies caller.

#### [MODIFY] [WebServer.cxx](file:///home/samurai/work/aimon/src/WebServer.cxx)
- Endpoints:
  - `GET /api/conversations`: Lists all conversations with status, participant agents, message counts, and timestamps.
  - `GET /api/conversations/active`: Returns currently active conversation (if any) and full message stream.
  - `GET /api/messages`: Returns chronological message records (optional `convo_id` parameter).
  - `POST /api/conversations/terminate`: Allows operator to terminate from web UI button.

---

### Component 3: CLI Console Operator & Monitoring (`NcursesConsole`)

#### [MODIFY] [NcursesConsole.hxx](file:///home/samurai/work/aimon/include/NcursesConsole.hxx) & [NcursesConsole.cxx](file:///home/samurai/work/aimon/src/NcursesConsole.cxx)
- **Approval Alert Box**:
  - Rendered in `_cmdWin` when a new conversation request arrives:
    ```text
    ┌─── [CONVERSATION REQUEST: convo-1] ──────────────────────────────────────┐
    │ Initiator: Antigravity IDE (Gemini)                                      │
    │ Target   : Cursor Grok (amba-virt)                                       │
    │ Topic/Msg: "Can we coordinate on amba-virt server plan?"                 │
    │ Action   : Type 'approve 1' to authorize session, or 'reject 1'.         │
    └──────────────────────────────────────────────────────────────────────────┘
    ```
- **CLI Commands**:
  - `approve [id]`, `app [id]`: Authorize the conversation session. If no ID is passed and exactly 1 is pending, approves that session.
  - `reject [id] [reason]`, `rej [id]`: Reject the conversation request.
  - `terminate [id]`, `term [id]`, `end [id]`: Terminate the active inter-agent conversation. If no ID is passed and exactly 1 is active, terminates that active session.
  - `convo` / `conv`: Shows active and past conversations.
- **Live Stream in Middle Panel (`_cmdWin`)**:
  - When session is approved:
    `[18:50:10] [convo-1] [SESSION STARTED] Antigravity ⇄ Cursor Grok authorized by operator` (green).
  - When messages/replies are sent:
    `[18:50:12] [convo-1] [Antigravity -> Cursor Grok]: "Can we coordinate on amba-virt server plan?"` (cyan).
    `[18:51:04] [convo-1] [Cursor Grok -> Antigravity]: "Yes, let's sync on Phase 1 proto 3."` (magenta).
  - When terminated:
    `[18:55:00] [convo-1] [SESSION TERMINATED] Closed by operator.` (yellow/red).

---

### Component 4: Web Dashboard Live Monitoring (`web/`)

#### [MODIFY] [web/index.html](file:///home/samurai/work/aimon/web/index.html)
- Add "Inter-Agent Dialogue" card to dashboard:
  - Header:
    - Title: "Inter-Agent Dialogue: Antigravity ⇄ Cursor Grok".
    - Status Badge: `ACTIVE` (glowing green dot), `PENDING` (yellow dot), or `TERMINATED` (muted gray dot).
    - Terminate Button: Quick web button to terminate conversation.
  - Body:
    - Auto-scrolling chat feed container (`#convo-feed`).
    - Message count and duration timers.
    - Avatars and stylized speech bubbles (cyan for Antigravity, magenta for Cursor Grok).
  - Empty State: Explains that inter-agent conversations will stream live here upon operator authorization.

#### [MODIFY] [web/app.js](file:///home/samurai/work/aimon/web/app.js)
- `fetchConversations()`: polls `/api/conversations/active` and `/api/messages`.
- `renderConversation(convo)`:
  - Dynamically renders conversation bubbles with provider styling.
  - Updates status badge and terminate button visibility.
  - Handles auto-scrolling to newest message.
- Polls every 2.5s in the dashboard refresh cycle.

#### [MODIFY] [web/style.css](file:///home/samurai/work/aimon/web/style.css)
- Styling for `.conversation-card`, `.convo-feed`, `.convo-bubble`, `.convo-status-active`, `.convo-status-terminated`, and action buttons.

---

## Verification Plan

### Automated / Build Verification
- Compile using top-level `Makefile`:
  ```bash
  make -j$(nproc)
  ```
- Verify clean compilation with zero warnings or errors.

### Manual Verification
1. **Initial Session Approval**:
   - Antigravity calls `agent_send_message(target="Cursor Grok", message="Hello Cursor, starting amba-virt sync.")`.
   - Verify `aimon` CLI console displays `[CONVERSATION REQUEST: convo-1]`.
   - In `aimon` console, operator runs `approve 1` (or `approve`).
   - Verify console shows `[SESSION STARTED] Antigravity ⇄ Cursor Grok authorized by operator`.
   - Verify initial message delivers to Cursor Grok's inbox.
2. **Autonomous Autonomous Dialogue (No further approvals)**:
   - Cursor Grok replies via `agent_send_reply`.
   - Antigravity sends follow-up via `agent_send_message(target="Cursor Grok", message="Step 1 complete.")`.
   - Verify follow-up delivers **instantly without requiring operator approval**.
   - Verify both messages appear in real time on both CLI console (`_cmdWin`) and Web dashboard (`http://builder:3883/`).
3. **Operator Termination in `aimon`**:
   - In `aimon` console, operator runs `terminate 1` (or `terminate`).
   - Verify console displays: `[convo-1] [SESSION TERMINATED] Closed by operator.`
   - Verify Web dashboard status updates immediately to `TERMINATED`.
   - Try to send another message from Antigravity: verify it is rejected because session is terminated.

---

## License & Copyright

Copyright (C) 2026, Charles Chiou. All rights reserved.
