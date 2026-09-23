# Implementation Plan: CollabOrchestrator Busy-Wait 100% CPU Fix

## 1. Context & Motivation

During system inspection on host `builder`, process `aimon` (PID `4088603`) was observed consuming 100% CPU on a dedicated CPU core, accumulating over 32 hours of CPU runtime (`TIME+: 32:07`). 

Inspection of thread-level CPU metrics pinpointed thread `4088739` running in a pure user-space loop (`utime: 11558088 jiffies`, `stime: 15 jiffies`). Tracing the thread creation sequence and validating via dynamic toggling (`./build/aimon collab auto off` / `on`) confirmed that the tight spin loop resides in `CollabOrchestrator::orchestratorLoop()` in `src/CollabOrchestrator.cxx`.

### Root Cause
In `src/CollabOrchestrator.cxx` (lines 440–443):
```cpp
_cv.wait_for(lock, std::chrono::seconds(2), [&]() {
    return !_running || _stepRequested || _autoDrive;
});
```
In C++, `std::condition_variable::wait_for(lock, rel_time, pred)` evaluates `pred()`. If `pred()` returns `true`, `wait_for` returns immediately without waiting for the timeout.
Because `collaboration.autoDrive` is enabled (`true`) by default, the predicate evaluated to `true` on every single iteration. When no collaboration session is in progress (status != `IN_PROGRESS`), the thread executed zero operations, did not sleep, and instantly looped, spinning at 100% CPU.

---

## 2. Operational Constraints & Non-Disruption Directive

> [!CAUTION]
> **STRICT ZERO-DISRUPTION MANDATE**:
> - `aimon` runs inside persistent GNU `screen` session `aimon` on `builder`.
> - Never kill or delete the `screen` session. Human operators and developer tooling rely on attached session geometry.
> - The service restart protocol:
>   1. Trigger graceful shutdown inside the console: `screen -S aimon -X stuff "quit\n"`.
>   2. Wait for clean exit.
>   3. Recompile with `make -j$(nproc)`.
>   4. Relaunch inside the persistent screen session: `screen -S aimon -X stuff "./build/aimon daemon\n"`.
>   5. Verify health and CPU utilization.

---

## 3. Proposed Changes

### [aimon]

#### [MODIFY] [src/CollabOrchestrator.cxx](file:///home/samurai/work/aimon/src/CollabOrchestrator.cxx)
- In `orchestratorLoop()`, remove `_autoDrive` from the `_cv.wait_for` predicate:
  ```cpp
  _cv.wait_for(lock, std::chrono::seconds(2), [&]() {
      return !_running || _stepRequested;
  });
  ```
- Rationale:
  - When `_autoDrive` is enabled, the thread will now sleep for up to 2 seconds between turns (using 0% CPU in kernel wait) instead of spinning.
  - If `_stepRequested` is set or an explicit notification (`_cv.notify_all()`) is sent (e.g. on `setAutoDrive`, `stepTurn`, `runUntilConsensus`, `abortRun`, or turn completion), the condition variable wakes up immediately with zero latency.
  - If `_autoDrive` is disabled, the thread continues to sleep waiting for manual steps or shutdown.

---

## 4. Staged Execution Plan

### Step 1: Source Code Modification
- Edit `src/CollabOrchestrator.cxx` in `/home/samurai/work/aimon` to correct the `_cv.wait_for` predicate.

### Step 2: Native Build on Builder
- Run `make -j$(nproc)` in `/home/samurai/work/aimon`.
- Ensure clean linking with zero compiler warnings or errors.

### Step 3: Graceful Screen Service Restart
- Signal graceful shutdown to `aimon` inside GNU screen `aimon`: `screen -S aimon -X stuff "quit\n"`.
- Verify process PID `4088603` terminates cleanly.
- Relaunch the new binary inside the screen session: `screen -S aimon -X stuff "./build/aimon daemon\n"`.

### Step 4: Verification
- Verify `aimon` reconnects and starts successfully.
- Verify CPU utilization via `top -H -b -n 1 -p <new_pid>` is ~0.0% idle.
- Enable auto-drive via `./build/aimon collab auto on` and confirm CPU utilization **remains at 0.0%** (no busy-wait).
- Query health and target status via MCP tool `get_combined_ai_status` and `embdevenv_list_targets`.

---

## 5. Verification Plan

### Automated / Command Verification
```bash
# 1. Check thread-level CPU usage of aimon with auto on
./build/aimon collab auto on
top -H -b -n 1 -p $(pgrep -f "^./build/aimon") | head -n 15

# 2. Verify MCP status and gateway responsiveness
mcp_aimon_get_combined_ai_status
```
Expected result: Thread `CollabOrchestrator` consumes 0.0% CPU when idle with auto-drive enabled.

---

## 6. Execution & Verification Summary (Completed)

- **Source Edit**: Corrected `_cv.wait_for` predicate in `src/CollabOrchestrator.cxx`.
- **Compilation**: Built with `make -j$(nproc)` with zero warnings/errors.
- **Screen Restart**: Restarted cleanly inside GNU screen `aimon`. New PID: `2140451`.
- **Verification**:
  - `aimon` total process CPU: **0.0%**.
  - `CollabOrchestrator` thread CPU: **0.0%**.
  - Enabled `auto_drive = true` (`./build/aimon collab auto on`): verified CPU remains at **0.0%** (zero busy-waiting).
  - TCP Gateway port 3885: verified all three downstream services (`netmon`, `embdevenv`, `meshmon`) reconnected and active.

