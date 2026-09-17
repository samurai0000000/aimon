# TODO: Cursor–Gemini Hardware Orchestration via aimon

Copyright (C) 2026, Charles Chiou. All rights reserved.

## Status

Human-in-the-loop gateway works. Target: Cursor orchestrates Gemini through
aimon MCP; the human only Approve/Reject on interlocking steps. Every step is
transcribed to disk for later analysis.

Proven on 2026-09-17 N1-655 camera bring-up
(`plan/plan_phase1_camera_bringup.md`, `plan/gemini_stuck_report.md` in the
target workspace).

## What happened (this session)

The human sat between two IDE agents:

1. Cursor (Grok) planned, reviewed, and issued the next constrained command.
2. Gemini executed on the board and appended a checkpoint to
   `plan/gemini_stuck_report.md`.
3. The human copied Gemini’s report into Cursor and Cursor’s instructions
   back to Gemini.

That copy-paste hop is the bottleneck. aimon should own the hop.

### Iterations that must be encoded as a state machine

| # | Iteration | What we learned | Policy to encode |
|---|-----------|-----------------|------------------|
| 1 | DSP-driver patches as first move | Vendor `dsp.ko` was not the first defect | Never patch DSP/IAV until a frozen baseline fails |
| 2 | Option 1 DTB (3 GB IDSP, `cavalry@1` `reg`) | Watchdog hang at EQOS ~2.11s | No unproven DTB deploy without rollback + 90s SSH gate |
| 3 | New `n1_655_devkit.dts` vs stock `eve.dtb` | Stock DTB already had DevKit SerDes | Diff against the **booting** DTB, not a reconstructed source |
| 4 | `ambcma` default 1602 MB vs 1488 MB `iav@0` | `dsp.ko` probe -12 | Pin `ama_enable=1 dsp_buf_size=0x40000000` |
| 5 | Core IAV + `load_ucode` before camera | `/dev/iav` Init, no assert | Checkpoint: IAV stack separate from SerDes |
| 6 | SerDes + OS08A10 probe | Link B `0xC8`, chip on `vinc:1` | Checkpoint: hardware before 3A |
| 7 | PREVIEW without IQ lua | `aaa_iq_config.lua` missing; IAV stuck Activating Preview | Do not retry PREVIEW on a wedged state; cold reboot |
| 8 | 3A success then `orcvin_boot.c:1909` | Userspace IQ OK; VIN lua / orcvin boot still fails | Isolate lua `vinc_id` vs firmware; no DSP source edits yet |

Hard constraints that kept the board recoverable:

- No OS image flash of unproven images.
- No `/persist` autostart `insmod` (udev/systemd). Modules only after SSH.
- No `rmmod` / hot-reload after a failed probe; cold boot instead.
- Stop after every checkpoint. Silence is not approval.

## Current vs target topology

```text
TODAY (human gateway)
  Cursor  --chat-->  Human  --chat-->  Gemini
                         |
                    copy report.md

TARGET (aimon bus)
  Cursor (reviewer/orchestrator)
       |  MCP: propose_next / approve_gate / read_transcript
       v
  aimon  (lock, policy, transcript JSONL, dashboard Approve/Reject)
       |  MCP: dispatch_checkpoint / agent_wait_turn / inbox
       v
  Gemini (executor)  --appends-->  transcript + report.md
```

Existing aimon pieces to reuse, not replace:

- `CollabOrchestrator` + `AgentMessageBus` (`IN_PROGRESS`, `OPERATOR_REVIEW`,
  `CONSENSUS_REACHED`)
- MCP: `register_agent_task`, `list_active_tasks`, `agent_check_inbox`,
  `agent_send_reply`, `agent_collaborate`, `agent_wait_turn`
- Dashboard task list (`/api/tasks`)

Gaps:

- Collaboration today is **plan-document ping-pong**, not **hardware checkpoint
  execution**.
- `agent_check_inbox` still assumes a human operator console as the other party.
- No structured JSONL transcript (only markdown appends under
  `## Collaboration`).
- No policy engine for flash / boot-insmod / DTB / power-cycle.
- No per-board exclusive lock (`n1-655-devkit`).
- Dashboard has tasks, not Approve/Reject for interlocking gates.

## Roles

| Role | Actor | Allowed |
|------|--------|---------|
| Planner / reviewer | Cursor | Write/revise plan, review checkpoints, propose next command, never silent-continue |
| Executor | Gemini | Run one approved checkpoint, append evidence, stop |
| Interlock | Human (optional) | Approve/Reject on dashboard for gated ops only |
| Bus / recorder | aimon | Dispatch, lock, policy, transcript, notify both agents |

Gemini must not interpret a missing reply as go-ahead.

## State machine

```text
IDLE
  -> PLAN_READY          (Cursor wrote plan/<name>.md)
  -> DISPATCH            (aimon sends one checkpoint to Gemini)
  -> EXECUTING           (Gemini holds board lock)
  -> CHECKPOINT          (structured result written)
  -> REVIEW              (Cursor reviews via MCP)
       -> CONTINUE       (auto if policy allows)
       -> INTERLOCK      (human Approve/Reject)
       -> RECOVER        (rollback DTB / power cycle — always interlock)
       -> STOP           (assertion, hang, policy violation)
```

Auto-continue examples: `insmod` of an already-hashed module, hash verify,
`dmesg` scrape.

Always interlock: DTB write to `mmcblk0p4`, MCU `pwr off`, U-Boot DTB restore,
any flash, git commit.

## Transcription (required)

Per run, aimon writes:

- `runs/<run-id>/run.json` — plan path, actors, policy, start/end
- `runs/<run-id>/transcript.jsonl` — one object per event
- `runs/<run-id>/report.md` — human mirror (today: `gemini_stuck_report.md`)

JSONL event fields (minimum):

```text
ts, run_id, seq, actor (cursor|gemini|human|aimon),
kind (dispatch|command|result|review|interlock|policy_deny|recover),
checkpoint_id, commands[], exit_codes[], artifacts{hashes},
board{ssh, model, eth0, iav_state},
dmesg_excerpt, decision (continue|stop|recover|wait_human),
proposal_next
```

This is the analysis corpus. Markdown is secondary.

## MCP tools to add (sketch)

Keep existing collab tools. Add an **execution** namespace:

- `exec_start_run(plan_file, workspace, target_alias, policy_id)`
- `exec_dispatch_checkpoint(run_id, checkpoint_id, instructions)` — Gemini only
- `exec_submit_checkpoint(run_id, result_json)` — Gemini; also appends report.md
- `exec_review_checkpoint(run_id, seq, decision, next_instructions)` — Cursor
- `exec_interlock_wait(run_id)` — blocks until human Approve/Reject
- `exec_get_transcript(run_id, since_seq)`

Dashboard: one row per run, last checkpoint, **Approve** / **Reject** / **Abort**.
Reject must include a reason string that becomes the next Cursor review input.

## Policy pack (N1-655 camera)

Deny without interlock:

- `dd`, installer, `eve.img` flash, USB matrix flash
- writing udev/systemd/`/persist` autostart that `insmod`s
- replacing `/dev/mmcblk0p4/eve.dtb` (allow only after explicit Approve)
- `rmmod` after probe failure

Auto-allow after Cursor `CONTINUE`:

- `insmod` listed hashed `.ko` with pinned params
- `load_ucode`, `test_encode`, `test_aaa_service`
- sysfs GPIO export of 92/93/98/99

Stop-and-review triggers:

- no SSH at 90s after reboot
- `dsp_check_assertion`
- `Preallocate CMA fail`
- IAV stuck in `Activating Preview`
- VIN IRQ delta 0 after a claimed PREVIEW success

## Implementation iterations (aimon)

1. **Transcript sink** — JSONL + markdown mirror; no agent dispatch yet.
   Capture a replay of `gemini_stuck_report.md` sections 5–10 as fixture events.
2. **Interlock UI** — `OPERATOR_REVIEW` already exists; bind Approve/Reject
   buttons to `exec_interlock_wait` instead of chat paste.
3. **Dispatch to Gemini** — either:
   - keep Gemini as desktop agent parked on `agent_wait_turn` / inbox
     (`deferToDesktop` already exists), or
   - `AgentRunner` launches a Gemini/Antigravity proxy with a frozen prompt
     “execute one checkpoint, submit, stop”.
4. **Cursor as orchestrator** — Cursor holds the review loop: on each
   `CHECKPOINT`, call `exec_review_checkpoint`. Human out of the copy path.
5. **Policy engine** — command allow/deny lists per workspace.
6. **Board lock** — one `run_id` per `n1-655-devkit` SSH alias.
7. **Replay tool** — `aimon exec replay runs/<id>` prints the analysis timeline.

Do not wait for a perfect AgentRunner. Iterations 1–2 already remove the human
from logging; 3–4 remove the human from message passing.

## Out of scope

- Letting Gemini auto-revise the plan without Cursor review
- Auto-flash / auto-DTB after a hang
- Publishing this workflow into other project trees
