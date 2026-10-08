# Envelope 0 Findings (2026-10-08, read-only)

Plan: plan/plan_claude_usage_collector.md. No product code changed. Files created: this file, build/baseline-home/, build/throwaway-home/ (both under the gitignored build tree).

## 0.1 Transcript layout (verified)
- Main sessions: `<configDir>/projects/<project>/<session-uuid>.jsonl`.
- Subagent (sidechain) usage is in SEPARATE files: `<project>/<session-uuid>/subagents/agent-<id>.jsonl`.
- Over all 1614 unique messages: 0 sidechain-flagged lines in main files, 0 non-sidechain lines in subagent files, 0 message keys present in both main and subagent files. So subagent usage is additive, never double-counted, and a recursive `**/*.jsonl` walk covers it.
- Other dirs per session: `tool-results/` (not transcripts), `memory/` (not transcripts).
- The scan root contains projects from more than one working context under the same login; attribution is by config directory, so all of them are counted for the account. Cost split by project is available from the path (Oct, est.): aimon 6.61, alpine-mcp 61.52, netmon 0.52, one other-context project 7.67 (total 76.33 at the time of the run; the live session keeps adding).

## 0.2 Retention (partially verified)
- `~/.claude/settings.json` keys: permissions, model, theme. No retention/cleanup setting present.
- Claude Code docs checked (settings, data-usage, env-vars): no local transcript-cleanup setting found by search for `cleanupPeriodDays`, `retention`, `retain`. The data-usage page covers server-side retention only.
- Earliest transcript line: 2026-09-21T11:50Z; `claudeCodeFirstTokenDate` is 2026-09-21. So nothing has been deleted since first use (17 days of history). Whether a local cleanup exists beyond 17 days is NOT established. Design consequence stands: persist rows in the store so later deletion cannot erase history.

## 0.3 Permissions (verified)
- Daemon user `samurai` = owner of `~/.claude`. `.credentials.json` mode 600, `~/.claude` and `projects` mode 755 (Claude Code's own choice; not changed by us).

## 0.4 Claude client name
- Deferred. Informational only (nothing matches on it now). Needs a new Claude Code session connected to aimon's MCP.

## 0.5 Consumers of the status JSON (verified by reading)
- Android: uses `org.json` with `opt*` accessors (tolerant of extra keys); `QuotaParserTest` parses a fixed JSON sample. An added `claude` key cannot break it.
- ncurses: reads typed structs (`status.cursor...`); unaffected. Optional later: show Claude there (out of plan).
- Home Assistant Lovelace cards: no Claude references; unaffected. MQTT discovery for Claude is in the plan.

## 0.6 Baseline `make test` on the unmodified tree (verified)
- Command: `HOME=<empty dir> make test`. Exit 0, 32.8 s. All 12 listed binaries pass (CppUTest suites report OK; assert-based suites report ALL TESTS PASSED).
- Finding A: the existing `build/` is configured Debug. The Makefile creates a fresh build as `-DCMAKE_BUILD_TYPE=Release` (`-O3 -DNDEBUG`). Under NDEBUG the plain `assert()` tests (TestCursorCollector: 36, TestWebServerApi: 27) compile to nothing and would pass vacuously. Not fixed here (out of scope). New tests in this plan use CppUTest `CHECK`, not `assert`. The sanitized build must be Debug.
- Finding B: running `make test` writes an incident file under `$HOME/.local/state/aimon/incidents/` (process-monitor lifecycle test). With the real HOME this lands in the live user's state directory. Pre-existing; gate runs will do the same unless the user prefers an isolated HOME.

## 0.7 Throwaway daemon (verified)
- `HOME=<dir>` isolates every state path (`PathUtils::expandHome` reads `$HOME`; telemetry, history, config, state all follow).
- Working invocation: `HOME=build/throwaway-home ./build/aimon daemon --config <that home>/.config/aimon/config.json --port 3893 --gateway-disable --no-ncurses`, with a config that disables antigravity and cursor discovery, MQTT, and the supervisor (so it cannot probe or restart real services).
- Result: listened on 3893, served `/sse` (HTTP 200), stopped cleanly on SIGINT. Live files (`history.db`, `telemetry.db`, `config.json` under `~/.config/aimon`) have identical mtime/size before and after. Live daemon (pid in screen, port 3883) unaffected.
- Note: the throwaway daemon binds 0.0.0.0 (no `--host` restriction used); future runs should pass `--host 127.0.0.1`.
