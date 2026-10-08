#!/usr/bin/env python3
"""
mcp_call.py

Minimal MCP client over the SSE transport used by aimon: connects, initializes,
calls one tool and writes the text of the first content block to a file.
Test tooling only.

  mcp_call.py HOST PORT PROFILE TOOL OUTFILE

Copyright (C) 2026, Charles Chiou
"""
import http.client
import json
import queue
import sys
import threading


def main(argv):
    if len(argv) != 6:
        print(__doc__)
        return 2
    host, port, profile, tool, outfile = argv[1], int(argv[2]), argv[3], argv[4], argv[5]
    events = queue.Queue()

    def reader(resp):
        ev, data = None, []
        for raw in resp:
            line = raw.decode().rstrip('\r\n')
            if line.startswith('event:'):
                ev = line[6:].strip()
            elif line.startswith('data:'):
                data.append(line[5:].strip())
            elif line == '' and ev:
                events.put((ev, '\n'.join(data)))
                ev, data = None, []

    conn = http.client.HTTPConnection(host, port, timeout=60)
    conn.request('GET', '/sse?profile=' + profile, headers={'Accept': 'text/event-stream'})
    resp = conn.getresponse()
    threading.Thread(target=reader, args=(resp,), daemon=True).start()
    ev, endpoint = events.get(timeout=10)
    if ev != 'endpoint':
        print('unexpected first event: ' + ev, file=sys.stderr)
        return 1

    def post(msg):
        c = http.client.HTTPConnection(host, port, timeout=30)
        c.request('POST', endpoint, body=json.dumps(msg), headers={'Content-Type': 'application/json'})
        r = c.getresponse()
        r.read()
        c.close()
        return r.status

    def rpc(i, method, params=None):
        msg = {'jsonrpc': '2.0', 'id': i, 'method': method}
        if params is not None:
            msg['params'] = params
        post(msg)
        _, data = events.get(timeout=60)
        return json.loads(data)

    rpc(1, 'initialize', {'protocolVersion': '2024-11-05', 'capabilities': {},
                          'clientInfo': {'name': 'mcp_call', 'version': '0'}})
    post({'jsonrpc': '2.0', 'method': 'notifications/initialized'})
    res = rpc(2, 'tools/call', {'name': tool, 'arguments': {}})
    if 'result' not in res:
        print('tool call failed: %s' % json.dumps(res)[:300], file=sys.stderr)
        return 1
    text = res['result']['content'][0]['text']
    with open(outfile, 'w', encoding='utf-8') as handle:
        handle.write(text)
    print('tool %s returned %d characters' % (tool, len(text)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
