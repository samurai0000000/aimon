#!/usr/bin/env python3
#
# mobile_permission_relay.py
#
# Copyright (C) 2026, Charles Chiou
#
# Lightweight CLI and PreToolUse hook relay that forwards tool execution
# approval requests to aimon's native C++ MobileGateway (/api/approvals/request)
# and polls (/api/approvals/status) until approved, denied, or expired.
#

import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request


def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        description="Relay tool execution approval requests to aimon MobileGateway."
    )
    parser.add_argument(
        "--agent",
        default=None,
        help="Agent identifier (default: from hook payload or 'claude-code').",
    )
    parser.add_argument(
        "--tool",
        "--action-type",
        dest="action_type",
        default=None,
        help="Tool or action type (default: from hook payload or 'run_command').",
    )
    parser.add_argument(
        "--command",
        default=None,
        help="Command string or action description.",
    )
    parser.add_argument(
        "--details",
        default=None,
        help="Additional context, working directory, or session details.",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=None,
        help="Approval timeout in seconds (default: 60).",
    )
    parser.add_argument(
        "--url",
        default=None,
        help="Base URL of aimon daemon (default: $AIMON_URL or http://127.0.0.1:3883).",
    )
    parser.add_argument(
        "--poll-interval",
        type=float,
        default=1.0,
        help="Polling interval in seconds (default: 1.0).",
    )
    return parser.parse_args(argv)


def read_hook_stdin():
    if sys.stdin is None or sys.stdin.isatty():
        return {}
    try:
        raw = sys.stdin.read()
        if not raw or not raw.strip():
            return {}
        parsed = json.loads(raw)
        if isinstance(parsed, dict):
            return parsed
    except Exception as exc:
        print(f"Warning: failed to parse stdin JSON payload: {exc}", file=sys.stderr)
    return {}


def extract_request_fields(args, payload):
    agent = (
        args.agent
        or payload.get("agent")
        or payload.get("agent_type")
        or payload.get("session_id")
        or os.environ.get("AIMON_AGENT")
        or "claude-code"
    )

    action_type = (
        args.action_type
        or payload.get("tool_name")
        or payload.get("action_type")
        or payload.get("tool")
        or "run_command"
    )

    command = args.command
    if command is None:
        tool_input = payload.get("tool_input")
        if isinstance(tool_input, dict):
            command = (
                tool_input.get("command")
                or tool_input.get("CommandLine")
                or tool_input.get("cmd")
            )
            if command is None and tool_input:
                command = json.dumps(tool_input, sort_keys=True)
        elif isinstance(tool_input, str):
            command = tool_input

        if command is None:
            command = payload.get("command") or payload.get("CommandLine") or ""

    details = args.details
    if details is None:
        details = (
            payload.get("cwd")
            or payload.get("details")
            or payload.get("workspace")
            or payload.get("session_id")
            or ""
        )

    timeout_seconds = args.timeout
    if timeout_seconds is None:
        raw_timeout = payload.get("timeout_seconds", 60)
        try:
            timeout_seconds = int(raw_timeout)
        except (TypeError, ValueError):
            timeout_seconds = 60
    if timeout_seconds <= 0:
        timeout_seconds = 60

    return {
        "agent": str(agent),
        "action_type": str(action_type),
        "command": str(command),
        "details": details if isinstance(details, (str, dict)) else str(details),
        "timeout_seconds": int(timeout_seconds),
    }


def http_json_request(url, method="GET", payload=None, timeout=10.0):
    headers = {
        "Accept": "application/json",
        "User-Agent": "aimon-permission-relay/1.0",
    }
    data = None
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
        headers["Content-Type"] = "application/json"

    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        body = resp.read().decode("utf-8")
        if not body:
            return {}
        return json.loads(body)


def main(argv=None):
    args = parse_args(argv)
    payload = {}
    if args.command is None:
        payload = read_hook_stdin()

    base_url = (
        args.url
        or os.environ.get("AIMON_URL")
        or "http://127.0.0.1:3883"
    ).rstrip("/")
    poll_interval = max(0.05, float(args.poll_interval))

    req_body = extract_request_fields(args, payload)
    timeout_seconds = req_body["timeout_seconds"]

    request_url = f"{base_url}/api/approvals/request"
    try:
        resp_data = http_json_request(
            request_url,
            method="POST",
            payload=req_body,
            timeout=min(10.0, float(timeout_seconds) + 5.0),
        )
    except urllib.error.HTTPError as exc:
        err_text = ""
        try:
            err_text = exc.read().decode("utf-8", errors="replace")
        except Exception:
            pass
        print(
            f"Approval request failed with HTTP {exc.code}: {err_text}",
            file=sys.stderr,
            flush=True,
        )
        return 2
    except Exception as exc:
        print(
            f"Approval request error communicating with {request_url}: {exc}",
            file=sys.stderr,
            flush=True,
        )
        return 2

    # If the server resolved synchronously inside /api/approvals/request
    initial_status = str(resp_data.get("status", "")).lower()
    if initial_status == "approved":
        print(json.dumps({"decision": "approve"}), flush=True)
        return 0
    if initial_status == "denied":
        print(
            f"Approval denied for {req_body['action_type']}: {req_body['command']}",
            file=sys.stderr,
            flush=True,
        )
        return 2
    if initial_status == "expired":
        print(
            f"Approval expired for {req_body['action_type']}: {req_body['command']}",
            file=sys.stderr,
            flush=True,
        )
        return 2

    request_id = (
        resp_data.get("request_id")
        or resp_data.get("id")
        or resp_data.get("approval_id")
    )
    if not request_id:
        print(
            f"Approval response missing request_id: {resp_data}",
            file=sys.stderr,
            flush=True,
        )
        return 2

    encoded_id = urllib.parse.quote(str(request_id), safe="")
    status_url = f"{base_url}/api/approvals/status?id={encoded_id}"
    deadline = time.monotonic() + float(timeout_seconds)

    while True:
        try:
            status_data = http_json_request(
                status_url,
                method="GET",
                timeout=min(5.0, float(timeout_seconds) + 2.0),
            )
        except Exception as exc:
            print(
                f"Error polling approval status ({status_url}): {exc}",
                file=sys.stderr,
                flush=True,
            )
            return 2

        status = str(status_data.get("status", "")).lower()
        verdict = str(status_data.get("verdict", "")).upper()

        if status == "approved" or verdict == "APPROVED":
            print(json.dumps({"decision": "approve"}), flush=True)
            return 0

        if status == "denied" or verdict == "DENIED":
            print(
                f"Approval denied for {req_body['action_type']}: {req_body['command']}",
                file=sys.stderr,
                flush=True,
            )
            return 2

        if status in ("expired", "not_found", "timed_out") or verdict == "TIMED_OUT":
            print(
                f"Approval expired for {req_body['action_type']}: {req_body['command']}",
                file=sys.stderr,
                flush=True,
            )
            return 2

        now = time.monotonic()
        if now >= deadline:
            print(
                f"Approval timed out after {timeout_seconds}s for {req_body['action_type']}: {req_body['command']}",
                file=sys.stderr,
                flush=True,
            )
            return 2

        sleep_dur = min(poll_interval, max(0.01, deadline - now))
        time.sleep(sleep_dur)


if __name__ == "__main__":
    sys.exit(main())
