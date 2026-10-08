#!/usr/bin/env python3
"""
cdp_page.py

Loads a page in headless Chromium through the DevTools protocol (standard
library only), waits until a JavaScript condition holds, then saves the
rendered DOM, a screenshot and any JavaScript errors. Used because
`--dump-dom` never returns in this environment. In this environment the
browser's own network stack cannot load any HTTP page (even a trivial local
one), so with --bridge every request the page makes is intercepted through the
DevTools Fetch domain and answered with the real response fetched here (only
GET requests to the page's own origin; everything else fails). The browser
still parses, lays out and runs the served HTML and JavaScript. Test tooling
only.

  cdp_page.py --browser PATH --url URL --dom OUT.html --png OUT.png --errors OUT.txt
              [--wait-js EXPR] [--wait-sec N] [--width W] [--height H] [--bridge]

Exit 0 when the page loaded and the wait condition became true.

Copyright (C) 2026, Charles Chiou
"""
import argparse
import base64
import json
import os
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import select
import urllib.parse
import urllib.request


class WebSocket:
    def __init__(self, host, port, path, timeout=30):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall(('GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                           'Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n'
                           % (path, host, port, key)).encode())
        resp = b''
        while b'\r\n\r\n' not in resp:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise IOError('connection closed during the websocket handshake')
            resp += chunk
        if b' 101 ' not in resp.split(b'\r\n', 1)[0]:
            raise IOError('websocket handshake refused: ' + resp.split(b'\r\n', 1)[0].decode(errors='replace'))
        self.rest = resp.split(b'\r\n\r\n', 1)[1]

    def _read(self, n):
        buf = b''
        while len(buf) < n:
            if self.rest:
                take, self.rest = self.rest[:n - len(buf)], self.rest[n - len(buf):]
                buf += take
                continue
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise IOError('connection closed')
            buf += chunk
        return buf

    def send(self, text):
        payload = text.encode()
        header = bytearray([0x81])
        n = len(payload)
        if n < 126:
            header.append(0x80 | n)
        elif n < 65536:
            header.append(0x80 | 126)
            header += struct.pack('>H', n)
        else:
            header.append(0x80 | 127)
            header += struct.pack('>Q', n)
        mask = os.urandom(4)
        header += mask
        self.sock.sendall(bytes(header) + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

    def recv(self):
        message = b''
        while True:
            b0, b1 = self._read(2)
            fin, opcode = b0 & 0x80, b0 & 0x0f
            length = b1 & 0x7f
            if length == 126:
                length = struct.unpack('>H', self._read(2))[0]
            elif length == 127:
                length = struct.unpack('>Q', self._read(8))[0]
            data = self._read(length)
            if opcode == 8:
                raise IOError('websocket closed by the browser')
            if opcode == 9:
                continue
            if opcode in (0, 1, 2):
                message += data
                if fin:
                    return message.decode()

    def readable(self, timeout):
        if self.rest:
            return True
        r, _, _ = select.select([self.sock], [], [], timeout)
        return bool(r)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


class Cdp:
    def __init__(self, ws):
        self.ws = ws
        self.next_id = 0
        self.events = []
        self.handler = None
        self.pending = {}

    def _event(self, msg):
        self.events.append(msg)
        if self.handler:
            self.handler(msg)

    def pump(self, timeout):
        """Process pending events for up to `timeout` seconds."""
        deadline = time.time() + timeout
        while time.time() < deadline and self.ws.readable(max(0.0, deadline - time.time())):
            msg = json.loads(self.ws.recv())
            if 'method' in msg:
                self._event(msg)

    def call(self, method, params=None, timeout=60):
        self.next_id += 1
        mid = self.next_id
        self.ws.send(json.dumps({'id': mid, 'method': method, 'params': params or {}}))
        deadline = time.time() + timeout
        while time.time() < deadline:
            # a nested call (made by an event handler) may already have read our reply
            msg = self.pending.pop(mid) if mid in self.pending else json.loads(self.ws.recv())
            if msg.get('id') == mid:
                if 'error' in msg:
                    raise RuntimeError('%s failed: %s' % (method, msg['error']))
                return msg.get('result', {})
            if 'id' in msg:
                self.pending[msg['id']] = msg
            elif 'method' in msg:
                self._event(msg)
        raise TimeoutError(method)

    def evaluate(self, expr):
        res = self.call('Runtime.evaluate', {'expression': expr, 'returnByValue': True, 'awaitPromise': False})
        if 'exceptionDetails' in res:
            raise RuntimeError('evaluation failed: ' + json.dumps(res['exceptionDetails'])[:300])
        return res['result'].get('value')


def free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    return port


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument('--browser', required=True)
    ap.add_argument('--url', required=True)
    ap.add_argument('--dom', required=True)
    ap.add_argument('--png', required=True)
    ap.add_argument('--errors', required=True)
    ap.add_argument('--wait-js', default='true')
    ap.add_argument('--wait-sec', type=int, default=30)
    ap.add_argument('--width', type=int, default=1920)
    ap.add_argument('--height', type=int, default=2600)
    ap.add_argument('--bridge', action='store_true')
    args = ap.parse_args(argv)

    port = free_port()
    profile = tempfile.mkdtemp(prefix='aimon_cdp_')
    proc = subprocess.Popen(
        [args.browser, '--no-sandbox', '--disable-gpu', '--user-data-dir=' + profile,
         '--remote-debugging-port=%d' % port,
         '--host-resolver-rules=MAP fonts.googleapis.com ~NOTFOUND, MAP fonts.gstatic.com ~NOTFOUND',
         'about:blank'],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    ws = None
    ok = False
    try:
        target = None
        for _ in range(100):
            try:
                tabs = json.load(urllib.request.urlopen('http://127.0.0.1:%d/json' % port, timeout=2))
                pages = [t for t in tabs if t.get('type') == 'page']
                if pages:
                    target = pages[0]
                    break
            except OSError:
                pass
            time.sleep(0.2)
        if not target:
            print('no browser page target appeared', file=sys.stderr)
            return 1
        ws = WebSocket('127.0.0.1', port, target['webSocketDebuggerUrl'].split(str(port), 1)[1])
        cdp = Cdp(ws)
        for domain in ('Page', 'Runtime', 'Log'):
            cdp.call(domain + '.enable')
        cdp.call('Emulation.setDeviceMetricsOverride',
                 {'width': args.width, 'height': args.height, 'deviceScaleFactor': 1, 'mobile': False})
        bridged = []
        if args.bridge:
            origin = urllib.parse.urlsplit(args.url)

            def serve(msg):
                if msg['method'] != 'Fetch.requestPaused':
                    return
                p = msg['params']
                rid, req = p['requestId'], p['request']
                u = urllib.parse.urlsplit(req['url'])
                same = (u.scheme, u.netloc) == (origin.scheme, origin.netloc)
                if not same or req['method'] != 'GET':
                    cdp.call('Fetch.failRequest', {'requestId': rid, 'errorReason': 'BlockedByClient'})
                    bridged.append((req['method'], req['url'], 'blocked'))
                    return
                try:
                    with urllib.request.urlopen(req['url'], timeout=15) as r:
                        status, headers, body = r.status, r.getheaders(), r.read()
                except urllib.error.HTTPError as e:
                    status, headers, body = e.code, e.headers.items(), e.read()
                except OSError as e:
                    cdp.call('Fetch.failRequest', {'requestId': rid, 'errorReason': 'ConnectionFailed'})
                    bridged.append(('GET', req['url'], 'failed: %s' % e))
                    return
                skip = {'transfer-encoding', 'connection', 'content-length', 'keep-alive', 'content-encoding'}
                hdrs = [{'name': k, 'value': v} for k, v in headers if k.lower() not in skip]
                cdp.call('Fetch.fulfillRequest', {'requestId': rid, 'responseCode': status,
                                                  'responseHeaders': hdrs,
                                                  'body': base64.b64encode(body).decode()})
                bridged.append(('GET', req['url'], status))

            cdp.handler = serve
            cdp.call('Fetch.enable', {'patterns': [{'urlPattern': '*'}]})
        cdp.call('Page.navigate', {'url': args.url})
        deadline = time.time() + args.wait_sec
        while time.time() < deadline:
            try:
                if cdp.evaluate('document.readyState') == 'complete' and cdp.evaluate(args.wait_js):
                    ok = True
                    break
            except RuntimeError:
                pass
            cdp.pump(0.3)
        cdp.pump(1.0)                                    # let the last render settle
        dom = cdp.evaluate('document.documentElement.outerHTML')
        open(args.dom, 'w', encoding='utf-8').write('<!DOCTYPE html>' + dom)
        shot = cdp.call('Page.captureScreenshot', {'format': 'png', 'captureBeyondViewport': True})
        open(args.png, 'wb').write(base64.b64decode(shot['data']))
        errors = []
        for ev in cdp.events:
            if ev['method'] == 'Runtime.exceptionThrown':
                d = ev['params']['exceptionDetails']
                errors.append('Uncaught: ' + (d.get('exception', {}).get('description') or d.get('text', '')))
            elif ev['method'] == 'Log.entryAdded' and ev['params']['entry'].get('level') == 'error' \
                    and 'Failed to load resource' not in ev['params']['entry'].get('text', ''):
                errors.append('Log error: ' + ev['params']['entry'].get('text', ''))
            elif ev['method'] == 'Runtime.consoleAPICalled' and ev['params'].get('type') == 'error':
                errors.append('console.error: ' + ' '.join(str(a.get('value', a.get('description', '')))
                                                          for a in ev['params'].get('args', [])))
        open(args.errors, 'w', encoding='utf-8').write('\n'.join(errors) + ('\n' if errors else ''))
        print('page loaded=%s dom=%d bytes png=%d bytes javascript errors=%d bridged requests=%d'
              % (ok, len(dom), len(base64.b64decode(shot['data'])), len(errors), len(bridged)))
        for method, url, result in bridged[:12]:
            print('  %s %s -> %s' % (method, url, result))
        return 0 if ok else 1
    finally:
        if ws:
            ws.close()
        try:
            os.killpg(proc.pid, signal.SIGTERM)
            proc.wait(timeout=5)
        except (ProcessLookupError, subprocess.TimeoutExpired):
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        subprocess.call(['rm', '-rf', profile])


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
