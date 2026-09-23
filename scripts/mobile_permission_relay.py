#!/usr/bin/env python3
"""
mobile_permission_relay.py

Universal Permission & Telemetry Interceptor for Antigravity & Cursor IDE.
Copyright (C) 2026, Charles Chiou
"""

import sys
import json
import os
import urllib.request
import urllib.error
import time

# Sensitive tools requiring mobile approval by default
SENSITIVE_TOOLS = {
    "run_command", "replace_file_content", "write_to_file",
    "multi_replace_file_content", "embdevenv_mcu_power",
    "embdevenv_flash_target", "firewall_block_ip", "firewall_unblock_ip"
}

def resolve_aimon_url():
    """Multi-tier endpoint discovery across Local Windows and Remote SSH."""
    if "AIMON_ENDPOINT" in os.environ:
        return os.environ["AIMON_ENDPOINT"].rstrip("/")
    candidates = [
        "http://127.0.0.1:3883",
        "http://builder:3883",
        "http://192.168.8.39:3883"
    ]
    for url in candidates:
        try:
            req = urllib.request.Request(f"{url}/api/status", headers={"User-Agent": "aimon-hook/1.0"})
            with urllib.request.urlopen(req, timeout=0.15) as res:
                if res.status == 200:
                    return url
        except Exception:
            continue
    return "http://127.0.0.1:3883"

def main():
    try:
        raw_input = sys.stdin.read()
    except Exception:
        sys.exit(0)

    if not raw_input or not raw_input.strip():
        # Passthrough if empty
        sys.exit(0)

    try:
        payload = json.loads(raw_input)
    except Exception:
        # Fallback to allow if non-json
        sys.exit(0)

    tool_name = payload.get("tool_name") or payload.get("tool") or ""
    tool_args = payload.get("tool_args") or payload.get("args") or {}
    agent_type = "antigravity" if "tool_name" in payload else "cursor"
    session_id = payload.get("session_id", "default_session")
    workspace = payload.get("cwd") or payload.get("workspace_path") or os.getcwd()

    aimon_url = resolve_aimon_url()

    # 1. Asynchronously emit PreToolUse telemetry event to AgentTelemetryDb
    try:
        telem_data = {
            "session_id": session_id,
            "agent_type": agent_type,
            "event_type": "TOOL_PRE_USE",
            "tool_name": tool_name,
            "status": "OK",
            "timestamp": int(time.time()),
            "details": {
                "workspace": workspace,
                "tool_args": tool_args
            }
        }
        telem_req = urllib.request.Request(
            f"{aimon_url}/api/telemetry/event",
            data=json.dumps(telem_data).encode("utf-8"),
            headers={"Content-Type": "application/json", "User-Agent": "aimon-hook/1.0"}
        )
        urllib.request.urlopen(telem_req, timeout=0.2)
    except Exception:
        pass

    # 2. Check if tool requires human approval
    if tool_name not in SENSITIVE_TOOLS:
        print(json.dumps({"decision": "allow"}))
        sys.exit(0)

    # 3. Request approval from MobileGateway
    req_body = {
        "agent_type": agent_type,
        "tool_name": tool_name,
        "workspace": workspace,
        "tool_args": tool_args,
        "reason": f"Execution of {tool_name} requires developer approval",
        "timeout_seconds": 120
    }

    try:
        req = urllib.request.Request(
            f"{aimon_url}/api/approvals/request",
            data=json.dumps(req_body).encode("utf-8"),
            headers={"Content-Type": "application/json", "User-Agent": "aimon-hook/1.0"}
        )
        with urllib.request.urlopen(req, timeout=125.0) as res:
            resp_data = json.loads(res.read().decode("utf-8"))
            if resp_data.get("verdict") == "APPROVED":
                print(json.dumps({"decision": "allow"}))
                sys.exit(0)
            else:
                print(json.dumps({"decision": "deny", "reason": "Denied by user on mobile companion"}))
                sys.exit(1)
    except Exception:
        # Fallback gracefully to allow so agent is never stuck
        print(json.dumps({"decision": "allow"}))
        sys.exit(0)

if __name__ == "__main__":
    main()
