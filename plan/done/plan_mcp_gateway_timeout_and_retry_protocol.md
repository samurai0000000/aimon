# Plan: MCP Gateway Dynamic Timeout & Transient Retry Protocol

- **Author**: Charles Chiou
- **Date**: 2026-09-20
- **Scope**: `aimon` MCP Gateway Hub, `intelligence/selfso` MCP tool rules, and downstream target control stability (`embdevenv`)
- **Status**: Proposed (Awaiting User Review)

---

## 1. Problem Statement & Motivation

During embedded target bringup (`amba-virt` on `n1-655-devkit`), an autonomous implementing agent halted execution per the **MCP Tool Integrity & Failure Protocol** due to two cascading issues:
1. **premature 15-Second Gateway Cutoff in `aimon`**:
   - `aimon`'s `TcpGateway::callTool()` had a hardcoded default timeout of **15 seconds** (`15000 ms`).
   - Downstream tools such as `embdevenv_console_catch_bootloader` and `embdevenv_console_expect` default to **30 seconds** (`timeout_sec = 30`), and `embdevenv_flash_target` takes up to 2 minutes.
   - Whenever any remote tool ran for more than 15 seconds, `aimon` unilaterally severed the call with:
     ```text
     Timed out waiting for subsystem 'embdevenv'
     ```
2. **Missing Precondition Check on Silent Serial Port**:
   - The target board was powered off (`powerOn: false`) following a 15-second U-Boot watchdog trip.
   - The agent invoked `embdevenv_console_catch_bootloader` without ensuring the board was powered on or setting `reboot_first: true`. The serial port remained silent, guaranteeing a 30-second timeout.
3. **Rigid Rule Paralyzing Agents on Transient Handshakes**:
   - `rules/mcp-tool-usage.md` mandated immediate halting on *any* error without distinguishing between transient transport reconnects (e.g. a 2-second reconnect after a daemon restart) and hard infrastructure defects.

---

## 2. Frozen Architectural Decisions

### Decision 1: Dynamic Gateway Timeouts in `aimon`
- When `McpServer::handleToolCall()` routes a tool to `_tcpGateway->callTool(...)`:
  - If `arguments` contains `timeout_sec` (integer), the gateway timeout is dynamically set to:
    $$\text{timeoutMs} = (\text{timeout\_sec} + 10) \times 1000$$
    This guarantees that the downstream subsystem's internal timeout always fires and reports cleanly before the gateway gives up.
  - If `timeout_sec` is not provided:
    - Bootloader catch & console expect tools (`embdevenv_console_catch_bootloader`, `embdevenv_console_expect`): default to **45,000 ms** (45 seconds).
    - Flashing tools (`embdevenv_flash_target`): default to **180,000 ms** (3 minutes).
    - General remote tool calls: default to **45,000 ms** (up from 15,000 ms).

### Decision 2: Target Hardware Precondition Protocol
- Before executing time-bounded serial console or bootloader tools, agents must verify or assert the target hardware state:
  - If target is powered off, power it on via `embdevenv_mcu_power(target=..., action="power_on")` or pass `reboot_first: true` to `embdevenv_console_catch_bootloader`.
  - Serial catch tools must never be invoked against an unpowered board.

### Decision 3: Single Transient Retry Allowance in Rule
- Update `mcp-tool-usage.md` (and Cursor `.mdc` mirror):
  - Allow **exactly ONE retry after a 3–5 second backoff** for transport-level errors (connection drop, SSE reconnecting, or temporary timeout).
  - If the second call fails, or if the failure is an unhandled logic/firmware crash, the agent must **STOP IMMEDIATELY and REPORT**.
  - The **strict prohibition on side-channel workarounds** (no raw sockets, telnet scripts, or direct curl) remains 100% in effect.

---

## 3. Staged Implementation Steps

### Phase 1: `aimon` Dynamic Timeout Engine [COMPLETED]
1. **[include/TcpGateway.hxx](file:///home/samurai/work/aimon/include/TcpGateway.hxx)**:
   - Changed default parameter in `callTool()` from `15000` to `45000` ms.
2. **[src/McpServer.cxx](file:///home/samurai/work/aimon/src/McpServer.cxx)**:
   - Calculates dynamic `timeoutMs` based on tool name and `arguments["timeout_sec"]`.
   - Passes computed `timeoutMs` to `_tcpGateway->callTool()`.

### Phase 2: Offline Automated Verification [COMPLETED]
1. Created unit & integration test harness **[test/TestGatewayTimeout.cxx](file:///home/samurai/work/aimon/test/TestGatewayTimeout.cxx)**.
2. Updated **`CMakeLists.txt`** and **`Makefile`** with `test_gateway_timeout` and `make test`.
3. Executed `make test` on ephemeral port `29885`:
   - Verified formula calculations for `timeout_sec`, flashing, and default cases.
   - Verified 300ms timeout expiration handling.
   - Verified fast response completion (< 1ms).
   - Verified socket disconnection during in-flight call handling.
   - All tests passed with 0 errors.
   - Running `aimon` daemon on port 3883/3885 was untouched throughout.

### Phase 3: Update AI Rules Across Intelligence Hub [COMPLETED]
1. **[intelligence/selfso/agents/rules/mcp-tool-usage.md](file:///home/samurai/work/intelligence/selfso/agents/rules/mcp-tool-usage.md)**:
   - Added Section 2.1: Precondition Verification.
   - Added Section 2.2: Single Transient Retry Protocol (3–5s backoff for transport drops).
2. **[intelligence/selfso/cursor/rules/mcp-tool-usage.mdc](file:///home/samurai/work/intelligence/selfso/cursor/rules/mcp-tool-usage.mdc)**:
   - Synchronized with agent rule for Cursor IDE parity.
3. **[intelligence/selfso/agents/GEMINI.md](file:///home/samurai/work/intelligence/selfso/agents/GEMINI.md)**:
   - Updated MCP tool failure protocol summary.

### Phase 4: Deployment & Live Verification [PENDING USER SIGNAL]
1. Awaiting explicit user confirmation before touching running `aimon` screen session.
2. Upon user signal:
   - Stop `aimon` in screen (`quit`).
   - Run `./build/aimon daemon --port 3883`.
   - Verify client reconnects.
