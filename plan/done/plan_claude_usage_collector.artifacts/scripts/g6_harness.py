#!/usr/bin/env python3
"""
g6_harness.py

Builds a local harness page from the dashboard files that the daemon served
over HTTP: the same index.html, style.css, app.js and qrcode.js, plus a small
script that answers the page's fetch() calls with the daemon's real JSON
responses. The only changes to index.html are removing the external web-font
links (which cannot load here) and adding the two harness scripts. Test
tooling only.

  g6_harness.py build --site DIR --api DIR

DIR/api holds one file per endpoint, named by the endpoint path with '/'
replaced by '_' (for example api_status.json for /api/status).

Copyright (C) 2026, Charles Chiou
"""
import argparse
import json
import os
import re
import sys

STUB = """(function () {
    const api = window.__aimonApi || {};
    window.fetch = function (input, init) {
        const path = new URL(String(input), 'http://aimon.invalid/').pathname;
        const method = (init && init.method) || 'GET';
        if (method !== 'GET' || !(path in api)) {
            return Promise.resolve(new Response('{}', {status: 404, headers: {'Content-Type': 'application/json'}}));
        }
        return Promise.resolve(new Response(api[path], {status: 200, headers: {'Content-Type': 'application/json'}}));
    };
})();
"""


def main(argv):
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('build')
    b.add_argument('--site', required=True)
    b.add_argument('--api', required=True)
    args = ap.parse_args(argv)

    api = {}
    for name in sorted(os.listdir(args.api)):
        if not name.endswith('.json'):
            continue
        path = '/' + name[:-5].replace('_', '/')
        text = open(os.path.join(args.api, name), encoding='utf-8').read()
        json.loads(text)                      # must be valid JSON
        api[path] = text
    with open(os.path.join(args.site, 'api.js'), 'w', encoding='utf-8') as handle:
        handle.write('window.__aimonApi = ' + json.dumps(api) + ';\n')
    with open(os.path.join(args.site, 'stub.js'), 'w', encoding='utf-8') as handle:
        handle.write(STUB)

    page = open(os.path.join(args.site, 'index.html'), encoding='utf-8').read()
    page, removed = re.subn(r'<link[^>]*(fonts\.googleapis\.com|fonts\.gstatic\.com)[^>]*>\s*', '', page)
    marker = '<script src="qrcode.js"></script>'
    if page.count(marker) != 1:
        print('qrcode.js script tag not found exactly once', file=sys.stderr)
        return 1
    page = page.replace(marker, '<script src="api.js"></script>\n    <script src="stub.js"></script>\n    ' + marker, 1)
    with open(os.path.join(args.site, 'harness.html'), 'w', encoding='utf-8') as handle:
        handle.write(page)
    print('harness built: %d endpoints stubbed, %d external font links removed' % (len(api), removed))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
