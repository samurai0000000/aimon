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

## Future Backlog

### Telemetry & Collector Enhancements
- [ ] Support Claude Desktop / Anthropic local token cache collector if official local daemon emerges.
- [ ] Add Prometheus `/metrics` exporter endpoint for Grafana integration alongside existing Home Assistant MQTT auto-discovery.
- [ ] Add historical trend graphs on web dashboard for weekly token consumption rate.
- [ ] Add configurable alert thresholds via desktop notifications (`libnotify`) when Cursor fast requests fall below 5%.
