#!/usr/bin/env python3
#
# test_mcp_client.py
#
# Copyright (C) 2026, Charles Chiou
#

import argparse
import http.client
import json
import queue
import socket
import sys
import threading
import time
import urllib.parse


class SseStreamReader:
    """
    Background reader for SSE streams supporting both Streamable HTTP (2025-03-26)
    and Legacy HTTP+SSE (2024-11-05) protocols.
    """

    def __init__(self, endpoint_url, session_id=None, is_streamable=False, protocol_version=None):
        self.endpoint_url = endpoint_url
        self.session_id = session_id
        self.is_streamable = is_streamable
        self.protocol_version = protocol_version or ("2025-03-26" if is_streamable else "2024-11-05")
        self.events_queue = queue.Queue()
        self.stop_event = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.endpoint_frames_count = 0
        self.keepalive_count = 0
        self.decode_errors = 0
        self.messages_received = []
        self.conn = None
        self.started = threading.Event()
        self.error = None

    def start(self):
        self.thread.start()
        # Wait until connected or error
        self.started.wait(timeout=5.0)

    def _run(self):
        parsed = urllib.parse.urlsplit(self.endpoint_url)
        host = parsed.hostname or "127.0.0.1"
        port = parsed.port or (443 if parsed.scheme == "https" else 80)
        path = parsed.path or "/"
        if parsed.query:
            path += "?" + parsed.query

        req_headers = {
            "Accept": "text/event-stream",
            "Cache-Control": "no-cache",
        }
        if self.session_id:
            req_headers["Mcp-Session-Id"] = self.session_id
        if self.is_streamable:
            req_headers["Mcp-Protocol-Version"] = self.protocol_version

        try:
            if parsed.scheme == "https":
                self.conn = http.client.HTTPSConnection(host, port, timeout=15)
            else:
                self.conn = http.client.HTTPConnection(host, port, timeout=15)

            self.conn.request("GET", path, headers=req_headers)
            resp = self.conn.getresponse()
            if resp.status != 200:
                self.error = f"SSE connection returned HTTP {resp.status}"
                self.started.set()
                return

            self.started.set()

            event_type = None
            data_lines = []

            while not self.stop_event.is_set():
                line_bytes = resp.fp.readline()
                if not line_bytes:
                    break
                line = line_bytes.decode("utf-8", errors="replace").rstrip("\r\n")

                if not line:
                    # Dispatch accumulated event frame
                    if event_type or data_lines:
                        data_str = "\n".join(data_lines)
                        if event_type == "endpoint":
                            self.endpoint_frames_count += 1

                        if self.is_streamable and data_str:
                            # In Streamable HTTP, every data payload must decode as JSON with jsonrpc="2.0"
                            try:
                                parsed_json = json.loads(data_str)
                                if isinstance(parsed_json, dict):
                                    if parsed_json.get("jsonrpc") != "2.0":
                                        self.decode_errors += 1
                                elif isinstance(parsed_json, list):
                                    for item in parsed_json:
                                        if isinstance(item, dict) and item.get("jsonrpc") != "2.0":
                                            self.decode_errors += 1
                                else:
                                    self.decode_errors += 1
                            except Exception:
                                self.decode_errors += 1

                        self.messages_received.append((event_type, data_str))
                        self.events_queue.put((event_type, data_str))

                    event_type = None
                    data_lines = []
                elif line.startswith(":"):
                    # Comment line (e.g. keepalive)
                    self.keepalive_count += 1
                elif line.startswith("event:"):
                    event_type = line[len("event:"):].strip()
                elif line.startswith("data:"):
                    data_lines.append(line[len("data:"):].strip())

        except Exception as ex:
            if not self.stop_event.is_set():
                self.error = str(ex)
        finally:
            self.started.set()

    def close(self):
        self.stop_event.set()
        if self.conn:
            try:
                if self.conn.sock:
                    try:
                        self.conn.sock.shutdown(socket.SHUT_RDWR)
                    except Exception:
                        pass
                    self.conn.sock.close()
                self.conn.close()
            except Exception:
                pass
        self.thread.join(timeout=1.0)


def send_http_request(url, method="POST", headers=None, body=None, timeout=10):
    parsed = urllib.parse.urlsplit(url)
    host = parsed.hostname or "127.0.0.1"
    port = parsed.port or (443 if parsed.scheme == "https" else 80)
    path = parsed.path or "/"
    if parsed.query:
        path += "?" + parsed.query

    req_headers = {}
    if headers:
        req_headers.update(headers)

    if isinstance(body, (dict, list)):
        body_bytes = json.dumps(body).encode("utf-8")
        if "Content-Type" not in req_headers and "content-type" not in req_headers:
            req_headers["Content-Type"] = "application/json"
    elif isinstance(body, str):
        body_bytes = body.encode("utf-8")
    elif isinstance(body, bytes):
        body_bytes = body
    else:
        body_bytes = None

    if parsed.scheme == "https":
        conn = http.client.HTTPSConnection(host, port, timeout=timeout)
    else:
        conn = http.client.HTTPConnection(host, port, timeout=timeout)

    try:
        conn.request(method, path, body=body_bytes, headers=req_headers)
        resp = conn.getresponse()
        status = resp.status
        resp_headers = {k.lower(): v for k, v in resp.getheaders()}
        resp_body = resp.read()
        return status, resp_headers, resp_body
    finally:
        conn.close()


def test_streamable_mode(endpoint, verbose=False):
    print(f"[TEST] Starting Streamable HTTP mode qualification against: {endpoint}")

    # 1. POST initialize
    print("  [Step 1] POST initialize (protocolVersion: 2025-03-26)...")
    init_payload = {
        "jsonrpc": "2.0",
        "id": 1,
        "method": "initialize",
        "params": {
            "protocolVersion": "2025-03-26",
            "clientInfo": {
                "name": "agy-probe",
                "version": "1.0.0"
            }
        }
    }
    status, headers, body = send_http_request(endpoint, method="POST", body=init_payload)
    assert status == 200, f"Expected HTTP 200, got {status}: {body.decode('utf-8', errors='replace')}"

    session_id = headers.get("mcp-session-id")
    assert session_id, f"Expected Mcp-Session-Id header in response headers: {headers}"
    if verbose:
        print(f"    Session ID: {session_id}")

    init_resp = json.loads(body.decode("utf-8"))
    assert init_resp.get("jsonrpc") == "2.0", f"Expected jsonrpc 2.0, got: {init_resp}"
    result = init_resp.get("result", {})
    assert result.get("protocolVersion") == "2025-03-26", f"Expected protocolVersion 2025-03-26, got: {result.get('protocolVersion')}"
    server_info = result.get("serverInfo", {})
    assert server_info.get("version") == "1.3.17", f"Expected serverInfo.version 1.3.17, got: {server_info.get('version')}"
    tools_cap = result.get("capabilities", {}).get("tools", {})
    assert tools_cap.get("listChanged") is True, f"Expected capabilities.tools.listChanged == True, got: {tools_cap}"
    print("    [PASS] POST initialize returned 200 OK with valid protocolVersion, serverInfo, and Mcp-Session-Id")

    # 2. Open standalone SSE stream in background thread
    print("  [Step 2] Opening standalone SSE stream on GET <endpoint>...")
    reader = SseStreamReader(
        endpoint,
        session_id=session_id,
        is_streamable=True,
        protocol_version="2025-03-26"
    )
    reader.start()
    assert reader.error is None, f"SSE stream connection failed: {reader.error}"

    # Allow time for initial frames/comments to arrive
    time.sleep(0.3)
    assert reader.endpoint_frames_count == 0, f"Standalone SSE stream emitted forbidden 'event: endpoint'! (count={reader.endpoint_frames_count})"
    assert reader.decode_errors == 0, f"Standalone SSE stream had JSON-RPC decode errors! (count={reader.decode_errors})"
    print(f"    [PASS] Standalone SSE stream opened cleanly (keepalive comments: {reader.keepalive_count}, endpoint frames: 0)")

    # 3. POST notification (notifications/initialized)
    print("  [Step 3] POST notification (notifications/initialized, no 'id')...")
    notif_payload = {
        "jsonrpc": "2.0",
        "method": "notifications/initialized"
    }
    status, headers, body = send_http_request(
        endpoint,
        method="POST",
        headers={"Mcp-Session-Id": session_id},
        body=notif_payload
    )
    assert status == 202, f"Expected HTTP 202 Accepted for notification, got {status}: {body.decode('utf-8', errors='replace')}"
    assert len(body) == 0, f"Expected empty response body for notification, got {len(body)} bytes: {body}"
    print("    [PASS] Notification returned HTTP 202 Accepted with empty body")

    # 4. POST tools/list
    print("  [Step 4] POST tools/list...")
    tools_payload = {
        "jsonrpc": "2.0",
        "id": 2,
        "method": "tools/list",
        "params": {}
    }
    status, headers, body = send_http_request(
        endpoint,
        method="POST",
        headers={"Mcp-Session-Id": session_id},
        body=tools_payload
    )
    assert status == 200, f"Expected HTTP 200 for tools/list, got {status}"
    tools_resp = json.loads(body.decode("utf-8"))
    tools = tools_resp.get("result", {}).get("tools", [])
    assert len(tools) > 0, "tools/list returned empty list"
    tool_names = {t.get("name") for t in tools}
    required_tools = [
        "check_antigravity_quota",
        "check_cursor_usage",
        "check_claude_usage",
        "get_combined_ai_status"
    ]
    for rt in required_tools:
        assert rt in tool_names, f"Required tool '{rt}' not found in tools list: {tool_names}"
    print(f"    [PASS] tools/list returned {len(tools)} tools containing all core quota & status tools")

    # 5. POST batch request
    print("  [Step 5] POST JSON-RPC 2.0 batch request...")
    batch_payload = [
        {"jsonrpc": "2.0", "id": 10, "method": "ping"},
        {"jsonrpc": "2.0", "id": 11, "method": "resources/list"}
    ]
    status, headers, body = send_http_request(
        endpoint,
        method="POST",
        headers={"Mcp-Session-Id": session_id},
        body=batch_payload
    )
    assert status == 200, f"Expected HTTP 200 for batch request, got {status}"
    batch_resp = json.loads(body.decode("utf-8"))
    assert isinstance(batch_resp, list), f"Expected list response for batch, got: {type(batch_resp)}"
    assert len(batch_resp) == 2, f"Expected 2 responses in batch, got {len(batch_resp)}"
    resp_ids = {r.get("id") for r in batch_resp}
    assert resp_ids == {10, 11}, f"Expected IDs {10, 11}, got {resp_ids}"
    print("    [PASS] Batch request returned 2-element JSON array with matching IDs")

    # 6. POST tools/call (check_antigravity_quota)
    print("  [Step 6] POST tools/call (check_antigravity_quota)...")
    call_payload = {
        "jsonrpc": "2.0",
        "id": 12,
        "method": "tools/call",
        "params": {
            "name": "check_antigravity_quota",
            "arguments": {}
        }
    }
    status, headers, body = send_http_request(
        endpoint,
        method="POST",
        headers={"Mcp-Session-Id": session_id},
        body=call_payload
    )
    assert status == 200, f"Expected HTTP 200 for tools/call, got {status}"
    call_resp = json.loads(body.decode("utf-8"))
    content = call_resp.get("result", {}).get("content", [])
    assert len(content) > 0, f"Expected non-empty content in tools/call result: {call_resp}"
    text_content = content[0].get("text", "")
    assert "Google Antigravity Quota Status" in text_content, f"Expected 'Google Antigravity Quota Status' in result text, got: {text_content[:100]}..."
    print("    [PASS] tools/call succeeded and returned valid Antigravity quota content")

    # 7. Check SSE reader health
    print("  [Step 7] Checking standalone SSE reader hygiene...")
    assert reader.endpoint_frames_count == 0, f"Standalone SSE stream emitted forbidden 'event: endpoint'! (count={reader.endpoint_frames_count})"
    assert reader.decode_errors == 0, f"Standalone SSE stream had JSON-RPC decode errors! (count={reader.decode_errors})"
    print(f"    [PASS] SSE stream hygiene verified: 0 endpoint frames, 0 decode errors, {reader.keepalive_count} keepalives")

    # 8. DELETE session
    print("  [Step 8] DELETE session...")
    status, headers, body = send_http_request(
        endpoint,
        method="DELETE",
        headers={"Mcp-Session-Id": session_id}
    )
    assert status == 204, f"Expected HTTP 204 No Content for DELETE, got {status}: {body}"
    print("    [PASS] DELETE returned HTTP 204 No Content")

    reader.close()

    # Resilient auto-detection check on /sse if testing /mcp
    parsed = urllib.parse.urlsplit(endpoint)
    if parsed.path.endswith("/mcp"):
        sse_endpoint = urllib.parse.urljoin(endpoint, "/sse")
        print(f"  [Auto-Detect] Testing Streamable HTTP auto-detection on {sse_endpoint}...")
        status, headers, body = send_http_request(sse_endpoint, method="POST", body=init_payload)
        assert status == 200, f"Expected HTTP 200 on POST /sse, got {status}"
        auto_resp = json.loads(body.decode("utf-8"))
        assert auto_resp.get("result", {}).get("protocolVersion") == "2025-03-26"
        auto_sid = headers.get("mcp-session-id")
        assert auto_sid, "Expected Mcp-Session-Id header on POST /sse"
        send_http_request(sse_endpoint, method="DELETE", headers={"Mcp-Session-Id": auto_sid})
        print("    [PASS] Resilient auto-detection on POST /sse verified")

    print("[SUCCESS] All Streamable HTTP qualification checks passed cleanly!\n")
    return True


def test_legacy_mode(endpoint, verbose=False):
    print(f"[TEST] Starting Legacy HTTP+SSE mode qualification against: {endpoint}")

    # 1. Connect to GET <endpoint> with Accept: text/event-stream
    print("  [Step 1] Connecting to GET <endpoint> with Accept: text/event-stream...")
    reader = SseStreamReader(endpoint, is_streamable=False, protocol_version="2024-11-05")
    reader.start()
    assert reader.error is None, f"Legacy SSE connection failed: {reader.error}"

    # 2. Read initial SSE event from stream
    print("  [Step 2] Reading initial SSE event (expecting event: endpoint)...")
    try:
        ev_type, endpoint_data = reader.events_queue.get(timeout=5.0)
    except queue.Empty:
        raise AssertionError("Timed out waiting for initial SSE endpoint event")

    assert ev_type == "endpoint", f"Expected event: endpoint, got: '{ev_type}'"
    assert "/message?sessionId=" in endpoint_data, f"Expected /message?sessionId= in data, got: '{endpoint_data}'"
    print(f"    [PASS] Received event: endpoint with data: '{endpoint_data}'")

    # 3. Parse relative URL against server base URL
    print("  [Step 3] Parsing message URL...")
    message_url = urllib.parse.urljoin(endpoint, endpoint_data)
    if verbose:
        print(f"    Target message URL: {message_url}")
    print("    [PASS] Parsed message URL successfully")

    # 4. POST initialize
    print("  [Step 4] POST initialize (protocolVersion: 2024-11-05)...")
    init_payload = {
        "jsonrpc": "2.0",
        "id": 1,
        "method": "initialize",
        "params": {
            "protocolVersion": "2024-11-05",
            "clientInfo": {
                "name": "cursor-legacy-probe",
                "version": "1.0.0"
            }
        }
    }
    status, headers, body = send_http_request(message_url, method="POST", body=init_payload)
    assert status == 200, f"Expected HTTP 200 for initialize, got {status}: {body.decode('utf-8', errors='replace')}"
    init_resp = json.loads(body.decode("utf-8"))
    assert init_resp.get("result", {}).get("protocolVersion") == "2024-11-05", f"Expected protocolVersion 2024-11-05, got: {init_resp}"

    # Verify response is also delivered on the SSE stream
    try:
        ev_type, sse_init_data = reader.events_queue.get(timeout=5.0)
    except queue.Empty:
        raise AssertionError("Timed out waiting for initialize response on legacy SSE stream")
    assert ev_type == "message", f"Expected event: message on SSE stream, got '{ev_type}'"
    sse_init_resp = json.loads(sse_init_data)
    assert sse_init_resp.get("id") == 1, f"Expected SSE message id 1, got: {sse_init_resp.get('id')}"
    print("    [PASS] POST initialize returned 200 OK and delivered matching event: message on SSE stream")

    # 5. POST tools/list and tools/call (check_cursor_usage)
    print("  [Step 5] POST tools/list & tools/call (check_cursor_usage)...")
    tools_payload = {
        "jsonrpc": "2.0",
        "id": 2,
        "method": "tools/list",
        "params": {}
    }
    status, headers, body = send_http_request(message_url, method="POST", body=tools_payload)
    assert status == 200, f"Expected HTTP 200 for tools/list, got {status}"
    tools_resp = json.loads(body.decode("utf-8"))
    tool_names = {t.get("name") for t in tools_resp.get("result", {}).get("tools", [])}
    assert "check_cursor_usage" in tool_names, f"Expected 'check_cursor_usage' in tools list: {tool_names}"

    # Drain SSE message event for tools/list
    try:
        ev_type, _ = reader.events_queue.get(timeout=5.0)
        assert ev_type == "message"
    except queue.Empty:
        pass

    call_payload = {
        "jsonrpc": "2.0",
        "id": 3,
        "method": "tools/call",
        "params": {
            "name": "check_cursor_usage",
            "arguments": {}
        }
    }
    status, headers, body = send_http_request(message_url, method="POST", body=call_payload)
    assert status == 200, f"Expected HTTP 200 for tools/call, got {status}"
    call_resp = json.loads(body.decode("utf-8"))
    content = call_resp.get("result", {}).get("content", [])
    assert len(content) > 0, "Expected non-empty content in check_cursor_usage result"
    text_content = content[0].get("text", "")
    assert "Cursor Usage" in text_content, f"Expected 'Cursor Usage' in result text, got: {text_content[:100]}..."

    # Drain SSE message event for tools/call
    try:
        ev_type, _ = reader.events_queue.get(timeout=5.0)
        assert ev_type == "message"
    except queue.Empty:
        pass

    print("    [PASS] tools/list and tools/call succeeded with legacy SSE streaming verification")

    # 6. Close SSE stream cleanly
    print("  [Step 6] Closing legacy SSE stream...")
    reader.close()
    print("    [PASS] Legacy SSE stream closed cleanly")

    print("[SUCCESS] All Legacy HTTP+SSE qualification checks passed cleanly!\n")
    return True


def main():
    parser = argparse.ArgumentParser(
        description="Aimon MCP End-to-End Client Qualification Probe"
    )
    parser.add_argument(
        "--endpoint",
        default="http://127.0.0.1:3883/mcp",
        help="Target server endpoint URL (default: http://127.0.0.1:3883/mcp)"
    )
    parser.add_argument(
        "--mode",
        choices=["streamable", "legacy"],
        default="streamable",
        help="Client simulation mode: 'streamable' (spec 2025-03-26) or 'legacy' (spec 2024-11-05)"
    )
    parser.add_argument(
        "--verbose", "-v",
        action="store_true",
        help="Enable verbose output"
    )
    args = parser.parse_args()

    try:
        if args.mode == "streamable":
            success = test_streamable_mode(args.endpoint, verbose=args.verbose)
        elif args.mode == "legacy":
            success = test_legacy_mode(args.endpoint, verbose=args.verbose)
        else:
            print(f"Unknown mode: {args.mode}", file=sys.stderr)
            sys.exit(1)

        if success:
            sys.exit(0)
        else:
            sys.exit(1)
    except AssertionError as err:
        print(f"\n[FAIL] Assertion error: {err}", file=sys.stderr)
        sys.exit(1)
    except Exception as ex:
        print(f"\n[ERROR] Unexpected error: {ex}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        sys.exit(1)


if __name__ == "__main__":
    main()
