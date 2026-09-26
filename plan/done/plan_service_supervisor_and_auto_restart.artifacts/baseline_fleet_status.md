# Baseline Fleet Status

- **Date**: 2026-09-26
- **Central Host**: `builder` (`127.0.0.1:3883`, `127.0.0.1:3885`)
- **Plan Reference**: [`plan/plan_service_supervisor_and_auto_restart.md`](../plan_service_supervisor_and_auto_restart.md)

## Active Satellite Fleet State
1. **`fox` (`192.168.8.245`)**:
   - `meshmon`: Running (v2.1.19, native aarch64 build).
   - Web: `16880`, TCP Shell: `16876`.
   - PID: `595091`.
   - Log: `~/.config/meshmon/meshmon.log`.
   - Status: Reachable and reconnected to `aimon:3885`.

2. **`rhino` (`192.168.8.30`)**:
   - `netmon`: Running on port `3884`. Connected to `aimon:3885`.
   - `embdevenv`: Running on port `3886`. Connected to `aimon:3885`.

3. **`builder` (`127.0.0.1`)**:
   - `aimon`: Central daemon active on port `3883` (web) and `3885` (gateway).
