#!/usr/bin/env python3
"""
webassets_sync.py

Keeps include/WebAssets.hxx (the dashboard assets embedded in the daemon) in
exact parity with the files under web/.

  webassets_sync.py check [--header FILE]   compare every embedded asset with its web/
                            file; exit 0 only when all are byte-identical
  webassets_sync.py write   regenerate the embedded literals from web/
  webassets_sync.py tamper --out FILE   copy the header with one byte of the embedded
                            index page changed (negative control for `check`)

Test and maintenance tooling; it is not part of the build.

Copyright (C) 2026, Charles Chiou
"""
import re
import sys

HEADER = 'include/WebAssets.hxx'
ASSETS = [('INDEX_HTML', 'web/index.html'), ('STYLE_CSS', 'web/style.css'),
          ('APP_JS', 'web/app.js'), ('QRCODE_JS', 'web/qrcode.js')]
DELIM_OPEN = 'R"raw_asset('
DELIM_CLOSE = ')raw_asset"'


def pattern(name):
    return re.compile(r'(inline const char\* %s = R"raw_asset\()(.*?)(\)raw_asset";)' % name, re.S)


def main(argv):
    header = HEADER
    args = list(argv[1:])
    if len(args) == 3 and args[1] == '--header' and args[0] == 'check':
        header = args[2]
        args = ['check']
    if len(args) == 3 and args[0] == 'tamper' and args[1] == '--out':
        text = open(HEADER, encoding='utf-8').read()
        i = text.index('<title>')
        open(args[2], 'w', encoding='utf-8').write(text[:i + 7] + 'X' + text[i + 8:])
        print('wrote a tampered copy to %s' % args[2])
        return 0
    if len(args) != 1 or args[0] not in ('check', 'write'):
        print(__doc__)
        return 2
    argv = [argv[0], args[0]]
    text = open(header, encoding='utf-8').read()
    bad = 0
    for name, path in ASSETS:
        src = open(path, encoding='utf-8').read()
        if DELIM_CLOSE in src:
            print('%s contains the raw string terminator; cannot embed' % path)
            return 2
        m = pattern(name).search(text)
        if not m:
            print('%s: literal not found in %s' % (name, HEADER))
            return 2
        same = m.group(2) == src
        if argv[1] == 'write' and not same:
            text = text[:m.start(2)] + src + text[m.end(2):]
            same = True
            print('%s: regenerated from %s (%d bytes)' % (name, path, len(src)))
        else:
            print('%s: %s (%d embedded, %d file)' % (name, 'IDENTICAL' if same else 'DIFFERENT',
                                                    len(m.group(2)), len(src)))
        bad += 0 if same else 1
    if argv[1] == 'write':
        open(header, 'w', encoding='utf-8').write(text)
        return 0
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
