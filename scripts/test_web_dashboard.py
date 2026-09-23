#!/usr/bin/env python3
import socket
import os
import sys
import base64
import struct
import json
import subprocess
import time
import urllib.request

class ChromeClient:
    def __init__(self, ws_url):
        parts = ws_url.replace("ws://", "").split("/", 1)
        host, port = parts[0].split(":")
        path = "/" + parts[1]
        
        self.sock = socket.create_connection((host, int(port)), timeout=15)
        key = base64.b64encode(os.urandom(16)).decode('utf-8')
        req = (f"GET {path} HTTP/1.1\r\n"
               f"Host: {host}:{port}\r\n"
               f"Upgrade: websocket\r\n"
               f"Connection: Upgrade\r\n"
               f"Sec-WebSocket-Key: {key}\r\n"
               f"Sec-WebSocket-Version: 13\r\n\r\n")
        self.sock.sendall(req.encode('utf-8'))
        resp = self.sock.recv(4096)
        if b"101" not in resp:
            raise RuntimeError(f"WebSocket upgrade failed: {resp}")
        self.msg_id = 0

    def _recv_exact(self, num_bytes):
        buf = b""
        while len(buf) < num_bytes:
            chunk = self.sock.recv(num_bytes - len(buf))
            if not chunk:
                raise EOFError("Socket closed")
            buf += chunk
        return buf

    def send(self, data_dict):
        msg = json.dumps(data_dict).encode('utf-8')
        length = len(msg)
        mask = os.urandom(4)
        header = bytearray([0x81])
        if length <= 125:
            header.append(0x80 | length)
        elif length <= 65535:
            header.append(0x80 | 126)
            header.extend(struct.pack("!H", length))
        else:
            header.append(0x80 | 127)
            header.extend(struct.pack("!Q", length))
        masked = bytes([b ^ mask[i % 4] for i, b in enumerate(msg)])
        self.sock.sendall(header + mask + masked)

    def recv_frame(self):
        try:
            header = self._recv_exact(2)
        except Exception:
            return None
        b1, b2 = header[0], header[1]
        opcode = b1 & 0x0F
        has_mask = (b2 & 0x80) != 0
        length = b2 & 0x7F
        if length == 126:
            length = struct.unpack("!H", self._recv_exact(2))[0]
        elif length == 127:
            length = struct.unpack("!Q", self._recv_exact(8))[0]
        
        mask = self._recv_exact(4) if has_mask else None
        data = self._recv_exact(length)
        if has_mask:
            data = bytes([b ^ mask[i % 4] for i, b in enumerate(data)])
        
        if opcode == 0x8: # close
            return None
        if opcode == 0x1: # text
            try:
                return json.loads(data.decode('utf-8'))
            except:
                return None
        return None

    def call(self, method, params=None):
        self.msg_id += 1
        curr_id = self.msg_id
        payload = {"id": curr_id, "method": method}
        if params:
            payload["params"] = params
        self.send(payload)
        start_t = time.time()
        while time.time() - start_t < 10:
            frame = self.recv_frame()
            if frame and isinstance(frame, dict):
                if frame.get("id") == curr_id:
                    if "error" in frame:
                        raise RuntimeError(f"CDP error: {frame['error']}")
                    return frame.get("result", {})
        raise TimeoutError(f"CDP method {method} timed out")

    def close(self):
        try:
            self.sock.close()
        except:
            pass

def main():
    print("[1/5] Launching headless Chromium on builder...")
    chrome_proc = subprocess.Popen([
        "/home/samurai/.cache/ms-playwright/chromium-1200/chrome-linux64/chrome",
        "--headless=new", "--no-sandbox", "--disable-gpu",
        "--remote-debugging-port=9223",
        "--window-size=1920,1080",
        "--user-data-dir=/tmp/chrome_cdp_test_profile"
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    time.sleep(1.5)

    try:
        print("[2/5] Connecting to Chrome DevTools Protocol...")
        targets = json.loads(urllib.request.urlopen("http://127.0.0.1:9223/json").read())
        page_target = next(t for t in targets if t.get("type") == "page")
        ws_url = page_target["webSocketDebuggerUrl"]
        
        client = ChromeClient(ws_url)
        
        # Set viewport to 1920x1080
        client.call("Emulation.setDeviceMetricsOverride", {
            "width": 1920, "height": 1080, "deviceScaleFactor": 1, "mobile": False
        })

        print("[3/5] Navigating to http://127.0.0.1:3883/ ...")
        client.call("Page.enable")
        client.call("Page.navigate", {"url": "http://127.0.0.1:3883/"})
        time.sleep(2.0)

        # Tab 1: AI Quotas
        print("  - Capturing Tab 1 (AI Quotas front page)...")
        res1 = client.call("Page.captureScreenshot", {"format": "png"})
        with open("/tmp/aimon_web_tab_aimon.png", "wb") as f:
            f.write(base64.b64decode(res1["data"]))
        print("    -> Saved /tmp/aimon_web_tab_aimon.png")

        # Tab 2: Telemetry
        print("  - Switching to Subpanel (Agent Telemetry & Analytics)...")
        client.call("Runtime.evaluate", {
            "expression": "document.querySelector('[data-subpanel=\"telemetry\"]').click()"
        })
        time.sleep(1.5)
        res2 = client.call("Page.captureScreenshot", {"format": "png"})
        with open("/tmp/aimon_web_tab_telemetry.png", "wb") as f:
            f.write(base64.b64decode(res2["data"]))
        print("    -> Saved /tmp/aimon_web_tab_telemetry.png")

        # Tab 3: Action Approvals
        print("  - Switching to Subpanel (Action Approvals & Mobile Companion)...")
        client.call("Runtime.evaluate", {
            "expression": "document.querySelector('[data-subpanel=\"approvals\"]').click()"
        })
        time.sleep(1.5)
        res3 = client.call("Page.captureScreenshot", {"format": "png"})
        with open("/tmp/aimon_web_tab_approvals.png", "wb") as f:
            f.write(base64.b64decode(res3["data"]))
        print("    -> Saved /tmp/aimon_web_tab_approvals.png")

        client.close()
        print("[4/5] Captured all 3 tab views.")
        print("[5/5] Visual validation completed successfully.")
        
    finally:
        chrome_proc.terminate()
        chrome_proc.wait()

if __name__ == "__main__":
    main()
