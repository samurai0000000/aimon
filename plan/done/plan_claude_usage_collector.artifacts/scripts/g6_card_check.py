#!/usr/bin/env python3
"""
g6_card_check.py

Compares the Claude card of the rendered dashboard (DOM dumped by a headless
browser) with the MCP tool text and the daemon status document. Integers come
from data attributes; formatted strings are compared as displayed. Test
tooling only.

  g6_card_check.py check --dom FILE --mcp FILE --status FILE [--records-out FILE]
  g6_card_check.py tamper-dom --in FILE --out FILE

Copyright (C) 2026, Charles Chiou
"""
import argparse
import html.parser
import json
import re
import sys


class Collector(html.parser.HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.by_id = {}
        self.stack = []
        self.models = []
        self.in_models = False
        self.cur_row = None

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        eid = a.get('id')
        entry = {'tag': tag, 'attrs': a, 'text': ''}
        if eid:
            self.by_id[eid] = entry
        self.stack.append(entry)
        if eid == 'claude-models-list':
            self.in_models = True
        if self.in_models and 'cat-name' in a.get('class', ''):
            self.cur_row = {'name': '', 'val': '', 'nano': None}
            self.models.append(self.cur_row)
            self.stack[-1]['role'] = 'name'
        if self.in_models and 'cat-val' in a.get('class', '') and self.cur_row is not None:
            self.cur_row['nano'] = a.get('data-nano')
            self.stack[-1]['role'] = 'val'

    def handle_endtag(self, tag):
        while self.stack:
            e = self.stack.pop()
            if e['tag'] == tag:
                break

    def handle_data(self, data):
        for e in self.stack:
            e['text'] += data
        if self.stack and self.cur_row is not None:
            role = self.stack[-1].get('role')
            if role == 'name':
                self.cur_row['name'] += data
            elif role == 'val':
                self.cur_row['val'] += data


def parse_mcp(text):
    out = {}
    m = re.search(r'\*\*Est\. [^*]*this cycle\*\*: (\$[\d.]+)(?: of (\$[\d.]+) \(([\d.]+)%\))?', text)
    if m:
        out['cycle_cost'], out['limit'], out['pct'] = m.group(1), m.group(2), m.group(3)
    m = re.search(r'\*\*Est\. [^*]*\*\*: (\$[\d.]+) \(5 h\) · (\$[\d.]+) \(7 d\) · (\$[\d.]+) \((\d+) d\)', text)
    if m:
        out['cost5h'], out['cost7d'], out['costw'], out['days'] = m.group(1), m.group(2), m.group(3), m.group(4)
    m = re.search(r'\*\*Tokens\*\*: (\S+) \(5 h\) · (\S+) \(7 d\) · (\S+) \((\d+) d\)', text)
    if m:
        out['tok5h'], out['tok7d'], out['tokw'] = m.group(1), m.group(2), m.group(3)
    out['models'] = re.findall(r'^\| `([^`]+)` \| (\d+) \| (\S+) \| (\$[\d.]+) \|$', text, re.M)
    return out


def check(args):
    dom = Collector()
    dom.feed(open(args.dom, encoding='utf-8').read())
    ids = dom.by_id
    if 'claude-card' not in ids:
        print('the Claude card is not in the rendered page', file=sys.stderr)
        return 1
    status = json.load(open(args.status, encoding='utf-8'))
    acct = status['claude']['accounts'][0]
    mcp = parse_mcp(open(args.mcp, encoding='utf-8').read())
    rows, bad = [], 0

    def rec(key, field, card, ref):
        nonlocal bad
        rows.append({'key': key, 'field': field, 'cpp': card, 'oracle': ref})
        if card != ref:
            bad += 1

    def attr(eid, name):
        return ids[eid]['attrs'].get(name) if eid in ids else None

    def text(eid):
        return ids[eid]['text'].strip() if eid in ids else None

    # integers: data attributes against the (oracle-verified) status document
    for key, win in (('5h', 'last_5h'), ('7d', 'last_7d'), ('window', 'window'), ('today', 'today')):
        rec('card:' + key, 'est_cost_nano', attr('claude-%s-cost' % key, 'data-nano'), str(acct[win]['est_cost_nano']))
        rec('card:' + key, 'total_tokens', attr('claude-%s-tokens' % key, 'data-tokens'), str(acct[win]['total_tokens']))
    main_nano = acct['cycle']['est_cost_nano'] if acct['cycle_configured'] else acct['window']['est_cost_nano']
    rec('card:main', 'est_cost_nano', attr('claude-main-value', 'data-nano'), str(main_nano))

    # displayed strings against the MCP text
    rec('card:5h', 'cost_text', text('claude-5h-cost'), mcp.get('cost5h'))
    rec('card:7d', 'cost_text', text('claude-7d-cost'), mcp.get('cost7d'))
    rec('card:window', 'cost_text', text('claude-window-cost'), mcp.get('costw'))
    rec('card:5h', 'tokens_text', text('claude-5h-tokens'), mcp.get('tok5h', '') + ' tokens')
    rec('card:7d', 'tokens_text', text('claude-7d-tokens'), mcp.get('tok7d', '') + ' tokens')
    rec('card:window', 'tokens_text', text('claude-window-tokens'), mcp.get('tokw', '') + ' tokens')
    rec('card:window', 'label_text', text('claude-window-label'), 'Last %s d' % mcp.get('days'))
    if acct['cycle_configured']:
        want = mcp.get('cycle_cost') + (' / ' + mcp['limit'] if mcp.get('limit') else '')
        rec('card:main', 'value_text', text('claude-main-value'), want)
        if mcp.get('pct'):
            rec('card:main', 'pct_text', text('claude-main-pct'), mcp['pct'] + '% of limit')
    rec('card:tier', 'badge_text', text('claude-tier-badge'),
        {'enterprise': 'Enterprise', 'personal': 'Personal'}.get(acct['tier'], 'Unknown tier'))
    rec('card:status', 'dot_online', 'dot-online' in ids['claude-status-dot']['attrs'].get('class', ''), acct['has_data'])
    rec('card:error', 'hidden_when_clean', 'hidden' in ids['claude-error']['attrs'].get('class', ''),
        not (acct['error_message'] or acct['warning']))

    # per-model rows: the top six, in MCP order
    want_models = mcp['models'][:6]
    rec('card:models', 'row_count', len(dom.models), len(want_models))
    for i, (name, _msgs, tokens, cost) in enumerate(want_models):
        if i < len(dom.models):
            row = dom.models[i]
            rec('card:model:%d' % i, 'name', row['name'].strip(), name)
            rec('card:model:%d' % i, 'value_text', row['val'].strip().replace('·', '|'), '%s | %s' % (cost, tokens))
    rec('card:footnote', 'says_estimate', 'Estimates' in (text('claude-footnote') or ''), True)

    if args.records_out:
        with open(args.records_out, 'w', encoding='utf-8') as handle:
            for r in rows:
                handle.write(json.dumps(r, sort_keys=True) + '\n')
    print('card check: %d fields, %d mismatches, %d model rows' % (len(rows), bad, len(dom.models)))
    return 1 if bad else 0


def tamper(args):
    text = open(args.infile, encoding='utf-8').read()
    new, n = re.subn(r'(id="claude-5h-cost"[^>]*data-nano=")(\d+)(")', lambda m: m.group(1) + str(int(m.group(2)) + 1) + m.group(3), text, count=1)
    if n != 1:
        new, n = re.subn(r'(data-nano=")(\d+)("[^>]*id="claude-5h-cost")', lambda m: m.group(1) + str(int(m.group(2)) + 1) + m.group(3), text, count=1)
    if n != 1:
        print('could not find the 5h cost element to tamper with', file=sys.stderr)
        return 1
    open(args.outfile, 'w', encoding='utf-8').write(new)
    print('tampered the 5 h cost of %s' % args.infile)
    return 0


def main(argv):
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('check')
    s.add_argument('--dom', required=True); s.add_argument('--mcp', required=True)
    s.add_argument('--status', required=True); s.add_argument('--records-out')
    s = sub.add_parser('tamper-dom')
    s.add_argument('--in', dest='infile', required=True); s.add_argument('--out', dest='outfile', required=True)
    args = ap.parse_args(argv)
    return check(args) if args.cmd == 'check' else tamper(args)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
