# TODO: aimon Development Roadmap

Copyright (C) 2026, Charles Chiou. All rights reserved.

## Completed Milestones

### 1. Gutted Orchestration & Interlock Subsystems (v1.0.3)
- [x] Completely purged all dead orchestration, agent chat, subagent runner, transcript recording, and execution interlock code.
- [x] Removed 19 dead orchestration MCP tools (`collab_*`, `exec_*`, `register_agent_task`, `list_active_tasks`, `agent_*`).
- [x] Cleaned web dashboard and Ncurses console of dead cards, waterfall timelines, and chat commands.
- [x] Blocked unauthorized direct REST calls (`/api/*`) with HTTP 403 Forbidden to enforce strict MCP primacy.

### 2. Scoped MCP Tool Profiling & Auto-Scoping
- [x] Implemented profile filtering (`core`, `embedded`, `network`, `mesh`, `all`).
- [x] Added automatic workspace detection during MCP client initialization based on root directory.
- [x] Added query parameter (`/sse?profile=...`), HTTP header (`X-Aimon-Profile`), and stdio CLI (`--profile=...`) overrides.
- [x] Verified zero tool leakage across profiles via stdio JSON-RPC 2.0 test suite.

---

### 3. Claude Code Usage Collector
- [x] Daemon reads Claude Code transcripts itself: incremental, deduplicated, per-account, with tier detection (Enterprise or personal) and a configurable spend limit and billing cycle.
- [x] Prices retrieved from the official pricing page (never embedded), cached, parsed strictly, versioned, with prompt-length tiers and quarantine of malformed rows.
- [x] `check_claude_usage` MCP tool, history samples, MQTT/Home Assistant sensors and a static dashboard card.
- [ ] Optional: authoritative account spend and limit from the claude.ai usage page (unofficial; needs a captured request).
- [ ] Optional: push source for Claude Code running on other machines; OpenTelemetry receiver.

---

## Future Backlog

### Telemetry & Collector Enhancements
- [ ] Add Prometheus `/metrics` exporter endpoint for Grafana integration alongside existing Home Assistant MQTT auto-discovery.
- [ ] Add historical trend graphs on web dashboard for weekly token consumption rate.
- [ ] Add configurable alert thresholds via desktop notifications (`libnotify`) when Cursor fast requests fall below 5%.
