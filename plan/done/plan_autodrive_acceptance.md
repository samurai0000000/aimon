# Auto-Drive Acceptance Test: Log Rotation for aimon

## 1. Objective

Acceptance fixture for the proxy-first orchestrator. The engineering content is
deliberately small so the run is cheap; the point of this document is to prove
that turns 1 through 4 execute with zero operator prompts.

## 2. Proposal & Technical Design

`aimon` writes daemon output to stdout inside its screen session, so nothing is
retained after a restart. Add optional file-based logging with size-based rotation.

### 2.1 Configuration Schema
We will introduce a new `"logging"` configuration section under `AimonConfig`, with the following keys:
- `logging.enabled` (boolean, default: `false`): Enables/disables file logging.
- `logging.log_path` (string, default: `"aimon.log"`): Base path for the active log file.
- `logging.rotation_threshold_bytes` (integer, default: `10485760` (10MB)): Maximum size of the active log file before it is rotated.
- `logging.retention_count` (integer, default: `5`): Number of rotated archive files to keep (e.g., `aimon.log.1` through `aimon.log.5`).

This maps to a new `LoggingConfig` structure in `include/ConfigManager.hxx`:
```cpp
struct LoggingConfig {
    bool enabled = false;
    std::string logPath = "aimon.log";
    size_t rotationThresholdBytes = 10 * 1024 * 1024;
    int retentionCount = 5;
};
```
Integrated into `AimonConfig`:
```cpp
struct AimonConfig {
    // ... other blocks
    LoggingConfig logging;
};
```

### 2.2 Component Location & Standalone Module
The logging mechanism will be fully encapsulated within a new, dedicated standalone module:
- Header file: `include/Logger.hxx`
- Implementation file: `src/Logger.cxx`

This module exposes a clean singleton interface or direct functions (e.g., `Logger::getInstance()`, `Logger::initialize(...)`, and `Logger::log(...)`) ensuring complete decoupling of logging concerns from the main daemon and configuration manager.

### 2.3 Rotation Mechanism
When size-based rotation triggers, the active log file is archived and rotated via the following steps:
1. Close the current file handle/stream (`std::ofstream`).
2. If `log_path.<retention_count>` exists, delete it.
3. For each index `i` from `retention_count - 1` down to 1:
   - If `log_path.i` exists, rename it to `log_path.(i+1)`.
4. Rename the active file `log_path` to `log_path.1`.
5. Re-open `log_path` in truncate/append mode.
6. Reset the in-memory size tracker `_currentLogSize` to `0`.

### 2.4 Synchronous Size Tracking & Thread-Safety (Avoiding `stat()`)
Calling `stat()` or `std::filesystem::file_size()` on every single log write introduces system call overhead. To prevent this, size-tracking is performed synchronously in-memory:
- **Initialization**: Upon daemon startup or logger initialization, the filesystem is queried exactly once to retrieve the initial file size of the active log file.
- **In-Memory Accumulator**: We maintain `size_t _currentLogSize` corresponding to the current byte size of the active log file.
- **Synchronous Write & Guard**: Since `aimon` logs concurrently from multiple background threads, all stream writes, size accumulation, size checks, and file rotation sequences are protected under a single `std::mutex _logMutex`.
- **$O(1)$ Inside Critical Section**: For every log write, the calling thread acquires `_logMutex`. Inside this critical section:
  - It writes the log string to the stream.
  - It flushes the stream to disk (`std::flush`) to ensure immediate visibility.
  - It increments `_currentLogSize` by the length of the formatted message.
  - It checks if `_currentLogSize` >= `rotation_threshold_bytes`. If true, it synchronously triggers the rotation mechanism (defined in Section 2.3) while still holding the lock.
This synchronous design removes any need for `std::atomic` variables or complex "double-checked locking" patterns, ensuring absolute safety, zero interleaving of log characters, and clean $O(1)$ size checking.

### 2.5 Error Handling & Fallback Strategy
To guarantee robustness and prevent daemon crashes or silent failure cascades, the logger employs explicit error handling:
- **Startup Permission/Creation Failures**: If the log file cannot be created or opened at startup (e.g., due to insufficient permissions or invalid directory paths):
  - Output a critical diagnostic error to `std::cerr`.
  - Disable file logging (`logging.enabled` is forced to `false` in memory).
  - Fall back gracefully to standard console logging (`stdout`/`stderr`), allowing the daemon to run without interruption.
- **Runtime Rotation / Rename Failures**: If file rotation fails (e.g., `rename()` fails due to disk space issues, file locks, or partition boundaries):
  - Output a warning to `std::cerr`.
  - Terminate the active file stream safely.
  - Temporarily fall back to logging directly to standard error (`stderr`) or disable file logging in memory to prevent recursive disk errors, ensuring continuous operations and visibility.

## 3. Open Questions (Resolved in Turn 1)

- **What configuration keys should control the log path, rotation threshold and retention count?**
  - *Resolution*: See Section 2.1. Keys defined are: `logging.enabled`, `logging.log_path`, `logging.rotation_threshold_bytes`, and `logging.retention_count`.
- **How should file size be tracked without a `stat()` call on every write?**
  - *Resolution*: See Section 2.4. Kept in a synchronous accumulator initialized once at startup and incremented by string length during each write within the mutex-protected block.
- **What guards concurrent writes during a rotation?**
  - *Resolution*: See Section 2.4. A dedicated `std::mutex _logMutex` guarantees mutually exclusive access to both the file stream and the rotation logic.

## 4. Verification

Verification of the log rotation feature will cover:
1. **Configuration Integration**:
   - Verify that JSON deserialization correctly loads and serializes the new `"logging"` schema, validating defaults.
2. **Rotation Correctness (Unit Tests)**:
   - Verify that log writes trigger rotation precisely when the threshold is crossed.
   - Verify that archives are shifted correctly (e.g., `aimon.log` -> `aimon.log.1` -> `aimon.log.2`) and that old files beyond the retention count are deleted.
3. **Efficiency Check**:
   - Verify that no system `stat()` calls occur on sequential write invocations.
4. **Concurrency & Stress Testing**:
   - Spin up multiple threads performing high-velocity logging to confirm complete file integrity, sequence preservation, and no deadlocks.

---

## License & Copyright

Copyright (C) 2026, Charles Chiou. All rights reserved.

## Collaboration

### Turn 1: Gemini CLI (Initiator)
- **Turn Started**: 2026-09-17T01:25:00Z
- **Model**: gemini-cli
- **Role**: Initiator
- **Re-work & Responses**:
  - Developed and proposed a complete logging configuration block (`logging.enabled`, `logging.log_path`, `logging.rotation_threshold_bytes`, and `logging.retention_count`).
  - Addressed efficiency concerns with a design for $O(1)$ in-memory size tracking (storing the starting file size and incrementing it by written string lengths), avoiding any system `stat()` calls on normal write pathways.
  - Resolved concurrency requirements using a double-checked locking pattern with a single `_logMutex` to safely serialize writes and perform multi-file rotation backups.
  - Formulated a comprehensive testing and verification plan including unit tests, integration validation, and high-concurrency stress testing.
- **Turn Finished**: 2026-09-17T01:35:00Z

### Turn 2: agent-cursor-windows (Reviewer)
- **Turn Started**: 2026-09-17T01:10:02+08:00
- **Model**: agent-cursor-windows
- **Role**: Reviewer
- **Feedback & Critique**:
  
The plan proposes a solid approach for log rotation, but requires revisions in a few architectural and design areas before it can be considered ready for implementation:

1. **Missing Component Location**: While the plan details configuration schema additions in `include/ConfigManager.hxx`, it completely omits where the actual logger implementation (the file handle, mutex, size accumulator, and rotation logic) will reside. Please specify whether this will be encapsulated in a new standalone module (e.g., `include/Logger.hxx` and `src/Logger.cxx`) or integrated into an existing component.
2. **Concurrency Design Contradiction**: Section 2.3 and 2.4 propose using `std::atomic<size_t>` for size tracking along with "Double-Checked Locking", while simultaneously asserting that a single `std::mutex _logMutex` will protect all stream writes. If stream writes must be mutually exclusive to prevent interleaved log characters, the size check and increment should happen synchronously inside the exact same critical section as the write. The atomic variable and double-checked locking are redundant and introduce unnecessary complexity; a simple `size_t` guarded by the mutex is much safer and simpler.
3. **Rotation Boundary Logic**: In Section 2.2, Step 2 states "If `i+1` exceeds `retention_count`, delete the oldest backup." Given the loop starts at `i = retention_count - 1`, `i+1` will exactly equal `retention_count`. The logic should simply state that the oldest backup `log_path.<retention_count>` is overwritten or explicitly removed, removing the ambiguous `exceeds` condition. 
4. **Missing Error Handling**: The plan lacks error handling for filesystem operations. You must define what happens if the daemon lacks permissions to open/create the log file on startup, or if a `rename()` fails during rotation (e.g., fallback to stderr, disable logging, or continue silently).

- **Status**: IN_PROGRESS
- **Turn Finished**: 2026-09-17T01:13:39+08:00

### Turn 3: Gemini CLI (Initiator)
- **Turn Started**: 2026-09-17T01:40:00Z
- **Model**: gemini-cli
- **Role**: Initiator
- **Re-work & Responses**:
  - Specified the location of the log rotation logic in a new dedicated standalone module (`include/Logger.hxx` and `src/Logger.cxx`).
  - Resolved the concurrency design contradiction by replacing `std::atomic<size_t>` and double-checked locking with a simple, robust synchronous size tracking and check inside the single `_logMutex` critical section.
  - Fixed the rotation boundary logic to explicitly delete the oldest archive file `log_path.<retention_count>` before executing the shift loop.
  - Added explicit, comprehensive error handling and fallback mechanisms for both startup permission failures and runtime rotation/rename failures.
- **Turn Finished**: 2026-09-17T01:45:00Z

### Turn 4: agent-cursor-windows (Reviewer)
- **Turn Started**: 2026-09-17T01:14:54+08:00
- **Model**: agent-cursor-windows
- **Role**: Reviewer
- **Feedback & Critique**:
  
- **Component Location**: The plan correctly specifies `include/Logger.hxx` and `src/Logger.cxx` as the dedicated standalone module for encapsulation.
- **Concurrency Design**: The contradictory atomic and double-checked locking mechanisms have been properly removed in favor of clean, synchronous size tracking and checks protected by a single `std::mutex`.
- **Rotation Boundary Logic**: The file renaming loop and the deletion of the oldest backup (`log_path.<retention_count>`) are now explicit, correct, and avoid previous ambiguities.
- **Error Handling**: The proposed fallbacks for startup permission failures and runtime rotation issues are well-defined and robust, ensuring the daemon continues to operate.
- **Verification**: The verification plan covers all necessary testing angles including integration, correctness, efficiency, and concurrency stress testing.

The plan successfully incorporates all previous feedback. It is completely sound and ready for implementation.

- **Status**: CONSENSUS_REACHED
- **Turn Finished**: 2026-09-17T01:15:26+08:00
