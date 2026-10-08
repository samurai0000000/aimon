#!/usr/bin/env python3
"""
claude_oracle.py

Independent oracle for the Claude usage plan (plan_claude_usage_collector).
Test tooling only; it is never shipped. It re-parses the Anthropic pricing page
and Claude Code transcripts with code that shares nothing with the C++
implementation, using the same documented integer arithmetic:

  * prices are integer nano-dollars per token ($P / MTok == P * 1000 n$/token);
    a price with more than 3 decimals is rejected;
  * fast-mode cache rates = round_half_up(fast_base * std_rate / std_input),
    derived from the base (or only) rate set;
  * a model with rows "(for prompts up to N tokens)" and "(for prompts over N
    tokens)" has a base and an upper rate set; a message whose prompt length
    (input + cache read + cache write 5m + cache write 1h) is over N uses the
    upper set; fast mode on an upper-tier message is unpriced;
  * row-level problems quarantine a model (unpriced); structural problems and
    more than 25% anomalous rows reject the page;
  * a transcript model id resolves to a catalog id when it is equal to it or
    equals it followed by "-" and exactly 8 digits (a date); nothing else;
  * per message key (message.id, requestId) the row with the strictly greatest
    output_tokens wins, the first seen wins ties;
  * versions: a message uses the latest catalog whose effective_from is not
    after the message time; earlier messages use the first catalog;
  * cost_nano = sum(tokens * rate) over input, 5m write, 1h write, read, output.

Usage:
  claude_oracle.py selftest --page FILE [--page2 FILE]
  claude_oracle.py prices --page FILE
  claude_oracle.py usage --root DIR (--page FILE | --versions FILE)
  claude_oracle.py compare --kind prices --cpp FILE --page FILE [--records-out FILE]
  claude_oracle.py compare --kind usage  --cpp FILE --root DIR (--page FILE | --versions FILE) [--records-out FILE]
  claude_oracle.py tamper --in FILE --out FILE

A versions file is JSON: [{"effective_from": EPOCH, "page": PATH}, ...].

Copyright (C) 2026, Charles Chiou
"""

import argparse
import calendar
import glob
import json
import os
import re
import sys
from decimal import Decimal, InvalidOperation

PRICE_RE = re.compile(r'^\$\s*([0-9]+(?:\.[0-9]+)?)\s*/\s*MTok$')
DATED_RE = re.compile(r'^-[0-9]{8}$')
TIER_RE = re.compile(r'\(for prompts (up to|over) ([0-9][0-9,]{0,11}) tokens\)')
MAX_FIELD = 256
MAX_TOKENS = 1000000000
MAX_EPOCH = 4102444800
TS_RE = re.compile(r'^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(\.[0-9]+)?Z$')

MAIN_HEADER = ['Model', 'Base input tokens', '5m cache writes', '1h cache writes',
               'Cache hits and refreshes', 'Output tokens']
FAST_HEADER = ['Model', 'Input', 'Output']
SET_FIELDS = ['input_nano', 'write5m_nano', 'write1h_nano', 'read_nano', 'output_nano']


class OracleError(Exception):
    pass


class Anomaly(Exception):
    """Row-level problem: quarantine the model."""


def price_to_nano(cell):
    cell = re.sub(r'<sup>.*?</sup>', '', cell).strip()
    m = PRICE_RE.match(cell)
    if not m:
        raise Anomaly('unparseable price cell: %r' % cell)
    try:
        value = Decimal(m.group(1)) * 1000
    except InvalidOperation:
        raise Anomaly('bad number: %r' % cell)
    if value != value.to_integral_value():
        raise Anomaly('price has more than 3 decimals: %r' % cell)
    if int(value) > 999999999:
        raise Anomaly('price out of range: %r' % cell)
    return int(value)


def plain_name_to_id(name):
    ident = re.sub(r'[ .]', '-', name.lower())
    if not re.match(r'^claude-[a-z0-9-]+$', ident) or ident.endswith('-') or len(ident) <= 7:
        raise Anomaly('unexpected model name: %r' % name)
    return ident


def split_row(line):
    return [c.strip() for c in line.strip().strip('|').split('|')]


def table_after(lines, heading, header):
    start = None
    for i, line in enumerate(lines):
        if line.strip() == heading:
            start = i
            break
    if start is None:
        raise OracleError('heading not found: %s' % heading)
    i = start + 1
    while i < len(lines) and not lines[i].lstrip().startswith('|'):
        if lines[i].startswith('#'):
            raise OracleError('no table under %s' % heading)
        i += 1
    if i >= len(lines) or split_row(lines[i]) != header:
        raise OracleError('unexpected table header under %s' % heading)
    i += 1
    sep = lines[i] if i < len(lines) else ''
    if not (sep.lstrip().startswith('|') and re.match(r'^[|:\- \t]+$', sep) and '-' in sep):
        raise OracleError('missing table separator under %s' % heading)
    i += 1
    rows = []
    while i < len(lines) and lines[i].lstrip().startswith('|'):
        cells = split_row(lines[i])
        if len(cells) != len(header):
            raise OracleError('row has %d cells, expected %d under %s' % (len(cells), len(header), heading))
        rows.append(cells)
        i += 1
    return rows


def rate_set(cells):
    inp, w5, w1, rd, out = (price_to_nano(c) for c in cells)
    if min(inp, w5, w1, rd, out) <= 0:
        raise Anomaly('non-positive price')
    if abs(4 * w5 - 5 * inp) * 100 > 5 * inp:
        raise Anomaly('5m write is not 1.25x input')
    if abs(w1 - 2 * inp) * 100 > 2 * inp:
        raise Anomaly('1h write is not 2x input')
    if rd > inp:
        raise Anomaly('cache read above input')
    return {'input_nano': inp, 'write5m_nano': w5, 'write1h_nano': w1, 'read_nano': rd, 'output_nano': out}


def parse_page(text):
    """Returns (catalog, quarantined). Raises OracleError on structural problems."""
    lines = text.split('\n')
    rows = table_after(lines, '## Model pricing', MAIN_HEADER)
    by_id = {}          # ident -> list of (tier, cells)
    anomalies = {}      # ident -> reason
    skipped_rows = 0
    for cells in rows:
        name = cells[0]
        m = TIER_RE.search(name)
        tier = None
        base = name
        if m:
            tier = (m.group(1), int(m.group(2).replace(',', '')))
            base = name[:m.start()]
        elif '(for prompts' in name:
            base = name[:name.index('(for prompts')]
            try:
                anomalies[plain_name_to_id(re.sub(r'\s*\(.*$', '', base).strip())] = 'unrecognized tier annotation'
            except Anomaly:
                skipped_rows += 1
            continue
        base = re.sub(r'\s*\(.*$', '', base).strip()
        try:
            ident = plain_name_to_id(base)
        except Anomaly:
            skipped_rows += 1
            continue
        by_id.setdefault(ident, []).append((tier, cells[1:]))

    catalog = {}
    for ident, entries in by_id.items():
        if ident in anomalies:
            continue
        try:
            tiers = [e[0] for e in entries]
            if any(t is None for t in tiers):
                if len(entries) != 1:
                    raise Anomaly('duplicate model')
                catalog[ident] = rate_set(entries[0][1])
            else:
                ups = [e for e in entries if e[0][0] == 'up to']
                overs = [e for e in entries if e[0][0] == 'over']
                if len(entries) != 2 or len(ups) != 1 or len(overs) != 1 or ups[0][0][1] != overs[0][0][1]:
                    raise Anomaly('inconsistent tier rows')
                base_set = rate_set(ups[0][1])
                upper = rate_set(overs[0][1])
                entry = dict(base_set)
                entry['tier_threshold_tokens'] = ups[0][0][1]
                for k, v in upper.items():
                    entry['upper_' + k] = v
                catalog[ident] = entry
        except Anomaly as exc:
            anomalies[ident] = str(exc)
    for ident in anomalies:
        catalog.pop(ident, None)
    if len(catalog) < 3:
        raise OracleError('fewer than 3 models')
    if (len(anomalies) + skipped_rows) * 4 > len(rows):
        raise OracleError('too many anomalous rows (%d of %d)' % (len(anomalies) + skipped_rows, len(rows)))

    for cells in table_after(lines, '### Fast mode pricing', FAST_HEADER):
        try:
            f_in, f_out = price_to_nano(cells[1]), price_to_nano(cells[2])
            if f_in <= 0 or f_out <= 0:
                raise Anomaly('non-positive fast price')
            idents = [plain_name_to_id(re.sub(r'\s*\(.*$', '', part).strip()) for part in cells[0].split(' / ')]
        except Anomaly:
            continue
        for ident in idents:
            if ident not in catalog:
                continue
            std = catalog[ident]
            rh = lambda a, b, c: (a * b + c // 2) // c
            std.update({
                'fast_input_nano': f_in,
                'fast_output_nano': f_out,
                'fast_write5m_nano': rh(f_in, std['write5m_nano'], std['input_nano']),
                'fast_write1h_nano': rh(f_in, std['write1h_nano'], std['input_nano']),
                'fast_read_nano': rh(f_in, std['read_nano'], std['input_nano']),
            })
    return catalog, dict(anomalies)


def resolve(catalog, model):
    if model in catalog:
        return model
    for ident in catalog:
        if model.startswith(ident) and DATED_RE.match(model[len(ident):]):
            return ident
    return None


def load_versions(args):
    if getattr(args, 'versions', None):
        spec = json.load(open(args.versions, encoding='utf-8'))
        versions = sorted(((int(v['effective_from']), parse_page(open(v['page'], encoding='utf-8').read())[0])
                           for v in spec), key=lambda x: x[0])
    else:
        versions = [(0, parse_page(open(args.page, encoding='utf-8').read())[0])]
    if not versions:
        raise OracleError('no price versions')
    return versions


def pick_version(versions, epoch):
    chosen = 0
    for i, (eff, _) in enumerate(versions):
        if eff <= epoch:
            chosen = i
    return versions[chosen][1]


def collect_rows(root, versions):
    """Deduplicated, costed messages: list of dicts with epoch, day, model, tokens, cost_nano, priced."""
    rows = {}
    skipped = 0
    for path in sorted(glob.glob(os.path.join(root, '**', '*.jsonl'), recursive=True)):
        with open(path, 'r', encoding='utf-8', errors='replace') as handle:
            for line in handle:
                try:
                    d = json.loads(line)
                except ValueError:
                    continue
                if not isinstance(d, dict) or d.get('type') != 'assistant':
                    continue
                m = d.get('message')
                if not isinstance(m, dict) or not isinstance(m.get('usage'), dict):
                    continue
                if m.get('model') == '<synthetic>':
                    continue
                mid, rid, ts, model = m.get('id'), d.get('requestId'), d.get('timestamp'), m.get('model')
                if not (isinstance(mid, str) and isinstance(rid, str) and isinstance(model, str)
                        and isinstance(ts, str) and TS_RE.match(ts)):
                    skipped += 1
                    continue
                if not all(0 < len(s) <= MAX_FIELD for s in (mid, rid, model)):
                    skipped += 1
                    continue
                if not (1 <= int(ts[5:7]) <= 12 and 1 <= int(ts[8:10]) <= 31 and int(ts[11:13]) <= 23
                        and int(ts[14:16]) <= 59 and int(ts[17:19]) <= 59):
                    skipped += 1
                    continue
                epoch = calendar.timegm((int(ts[0:4]), int(ts[5:7]), int(ts[8:10]),
                                         int(ts[11:13]), int(ts[14:16]), int(ts[17:19])))
                if not 0 < epoch < MAX_EPOCH:
                    skipped += 1
                    continue
                u = m['usage']
                cc = u.get('cache_creation')
                if isinstance(cc, dict):
                    c5, c1 = int(cc.get('ephemeral_5m_input_tokens', 0)), int(cc.get('ephemeral_1h_input_tokens', 0))
                else:
                    c5, c1 = int(u.get('cache_creation_input_tokens', 0)), 0
                row = {'day': ts[:10], 'epoch': epoch, 'model': model, 'speed': u.get('speed', 'standard'),
                       'input': int(u.get('input_tokens', 0)), 'output': int(u.get('output_tokens', 0)),
                       'cache_read': int(u.get('cache_read_input_tokens', 0)),
                       'cache_w5m': c5, 'cache_w1h': c1}
                if any(not 0 <= row[f] <= MAX_TOKENS for f in ('input', 'output', 'cache_read', 'cache_w5m', 'cache_w1h')):
                    skipped += 1
                    continue
                key = (mid, rid)
                if key not in rows or row['output'] > rows[key]['output']:
                    rows[key] = row
    out = []
    for row in rows.values():
        catalog = pick_version(versions, row['epoch'])
        ident = resolve(catalog, row['model'])
        price = catalog.get(ident) if ident else None
        fast = row['speed'] == 'fast'
        cost, priced = 0, False
        if price is not None:
            prompt = row['input'] + row['cache_read'] + row['cache_w5m'] + row['cache_w1h']
            upper = 'tier_threshold_tokens' in price and prompt > price['tier_threshold_tokens']
            if fast:
                if not upper and 'fast_input_nano' in price:
                    p = price
                    cost = (row['input'] * p['fast_input_nano'] + row['cache_w5m'] * p['fast_write5m_nano']
                            + row['cache_w1h'] * p['fast_write1h_nano'] + row['cache_read'] * p['fast_read_nano']
                            + row['output'] * p['fast_output_nano'])
                    priced = True
            else:
                pre = 'upper_' if upper else ''
                cost = (row['input'] * price[pre + 'input_nano'] + row['cache_w5m'] * price[pre + 'write5m_nano']
                        + row['cache_w1h'] * price[pre + 'write1h_nano'] + row['cache_read'] * price[pre + 'read_nano']
                        + row['output'] * price[pre + 'output_nano'])
                priced = True
        r = dict(row)
        r['cost_nano'] = cost
        r['priced'] = priced
        out.append(r)
    return out, skipped


def collect_usage(root, versions):
    rows, skipped = collect_rows(root, versions)
    agg = {}
    for row in rows:
        key = (row['day'], row['model'])
        a = agg.setdefault(key, {'day': row['day'], 'model': row['model'], 'messages': 0, 'input': 0,
                                 'output': 0, 'cache_read': 0, 'cache_w5m': 0, 'cache_w1h': 0,
                                 'cost_nano': 0, 'unpriced_messages': 0})
        a['messages'] += 1
        for f in ('input', 'output', 'cache_read', 'cache_w5m', 'cache_w1h'):
            a[f] += row[f]
        a['cost_nano'] += row['cost_nano']
        if not row['priced']:
            a['unpriced_messages'] += 1
    return [agg[k] for k in sorted(agg)], skipped


def read_jsonl(path):
    out = []
    with open(path, 'r', encoding='utf-8') as handle:
        for line in handle:
            line = line.strip()
            if line:
                out.append(json.loads(line))
    return out


def write_records(path, rows):
    if path:
        with open(path, 'w', encoding='utf-8') as handle:
            for r in rows:
                handle.write(json.dumps(r, sort_keys=True) + '\n')


def compare_prices(args):
    catalog, quarantined = parse_page(open(args.page, encoding='utf-8').read())
    expected = {(m, f): v for m, d in catalog.items() for f, v in d.items()}
    for ident in quarantined:
        expected[(ident, 'quarantined')] = 1
    got = {(r['model'], r['field']): r['cpp'] for r in read_jsonl(args.cpp)}
    rows, bad = [], 0
    for key in sorted(set(expected) | set(got)):
        o, c = expected.get(key), got.get(key)
        rows.append({'model': key[0], 'field': key[1], 'cpp': c, 'oracle': o})
        if o != c:
            bad += 1
    write_records(args.records_out, rows)
    print('prices compare: %d fields, %d mismatches, %d models, %d quarantined'
          % (len(rows), bad, len(catalog), len(quarantined)))
    return 1 if bad else 0


def compare_usage(args):
    versions = load_versions(args)
    oracle_rows, skipped = collect_usage(args.root, versions)
    expected = {(r['day'], r['model']): r for r in oracle_rows}
    got = {(r['day'], r['model']): r for r in read_jsonl(args.cpp)}
    fields = ['messages', 'input', 'output', 'cache_read', 'cache_w5m', 'cache_w1h',
              'cost_nano', 'unpriced_messages']
    rows, bad = [], 0
    for key in sorted(set(expected) | set(got)):
        for f in fields:
            o = expected[key][f] if key in expected else None
            c = got[key].get(f) if key in got else None
            rows.append({'day': key[0], 'model': key[1], 'field': f, 'cpp': c, 'oracle': o})
            if o != c:
                bad += 1
    write_records(args.records_out, rows)
    print('usage compare: %d keys, %d fields, %d mismatches, %d price versions, oracle skipped lines %d'
          % (len(expected), len(rows), bad, len(versions), skipped))
    return 1 if bad else 0


def tamper(args):
    """Negative-control helper: copy a C++ output file with one value changed."""
    rows = read_jsonl(args.infile)
    if not rows:
        raise OracleError('nothing to tamper with')
    for key in ('cost_nano', 'cpp'):
        if key in rows[0]:
            rows[0][key] += 1
            break
    else:
        raise OracleError('no numeric field to tamper with')
    with open(args.outfile, 'w', encoding='utf-8') as handle:
        for r in rows:
            handle.write(json.dumps(r, sort_keys=True) + '\n')
    print('tampered first row of %s' % args.infile)
    return 0


def status_compare(args):
    """Compare a daemon status document against an independent recount."""
    status = json.load(open(args.status, encoding='utf-8'))
    accounts = [a for a in status['claude']['accounts'] if a['name'] == args.account]
    if len(accounts) != 1:
        raise OracleError('account %r not found in the status document' % args.account)
    acct = accounts[0]
    versions = [(0, parse_page(open(args.page, encoding='utf-8').read())[0])]
    rows, _ = collect_usage(args.root, versions)
    out, bad = [], 0

    def check(key, field, cpp, oracle):
        nonlocal bad
        out.append({'key': key, 'field': field, 'cpp': cpp, 'oracle': oracle})
        if cpp != oracle:
            bad += 1

    names = [('messages', 'messages'), ('input', 'input_tokens'), ('output', 'output_tokens'),
             ('cache_read', 'cache_read_tokens'), ('cache_w5m', 'cache_write_5m_tokens'),
             ('cache_w1h', 'cache_write_1h_tokens'), ('cost_nano', 'est_cost_nano'),
             ('unpriced_messages', 'unpriced_messages')]
    total, by_model, by_day = {}, {}, {}
    for r in rows:
        for src, _ in names:
            total[src] = total.get(src, 0) + r[src]
            by_model.setdefault(r['model'], {})
            by_model[r['model']][src] = by_model[r['model']].get(src, 0) + r[src]
        d = by_day.setdefault(r['day'], {'messages': 0, 'tokens': 0, 'cost_nano': 0})
        d['messages'] += r['messages']
        d['tokens'] += r['input'] + r['output'] + r['cache_read'] + r['cache_w5m'] + r['cache_w1h']
        d['cost_nano'] += r['cost_nano']
    for src, dst in names:
        check('window', dst, acct['window'].get(dst), total.get(src, 0))
    models = {m['model']: m for m in acct['models']}
    for model in sorted(set(models) | set(by_model)):
        for src, dst in names:
            check('model:' + model, dst, models.get(model, {}).get(dst) if model in models else None,
                  by_model[model][src] if model in by_model else None)
    daily = {d['day_str']: d for d in acct['daily']}
    for day in sorted(set(daily) | set(by_day)):
        for field, dst in (('messages', 'messages'), ('tokens', 'tokens'), ('cost_nano', 'est_cost_nano')):
            check('day:' + day, dst, daily[day][dst] if day in daily else None,
                  by_day[day][field] if day in by_day else None)

    # every time window, from the message timestamps and the window bounds the daemon reported
    msgs, _ = collect_rows(args.root, versions)
    window_keys = [('last_5h', 'last_5h'), ('last_7d', 'last_7d'), ('today', 'today'), ('window', 'window')]
    if acct.get('cycle_configured'):
        window_keys.append(('cycle', 'cycle'))
    for key, label in window_keys:
        w = acct[key]
        sel = [m for m in msgs if w['from_epoch'] <= m['epoch'] < w['to_epoch']]
        tokens = sum(m['input'] + m['output'] + m['cache_read'] + m['cache_w5m'] + m['cache_w1h'] for m in sel)
        check('win:' + label, 'messages', w['messages'], len(sel))
        check('win:' + label, 'total_tokens', w['total_tokens'], tokens)
        check('win:' + label, 'est_cost_nano', w['est_cost_nano'], sum(m['cost_nano'] for m in sel))
        check('win:' + label, 'unpriced_messages', w['unpriced_messages'], sum(1 for m in sel if not m['priced']))

    cred = json.load(open(args.creds, encoding='utf-8')).get('claudeAiOauth', {})
    sub_type = cred.get('subscriptionType') or ''
    rate_tier = cred.get('rateLimitTier') or ''
    want = 'enterprise' if sub_type.lower() == 'enterprise' else ('personal' if sub_type else 'unknown')
    check('tier', 'tier', acct['tier'], want)
    check('tier', 'raw_subscription_type', acct['raw_subscription_type'], sub_type)
    check('tier', 'raw_rate_limit_tier', acct['raw_rate_limit_tier'], rate_tier)
    if args.second:
        second = json.load(open(args.second, encoding='utf-8'))
        a2 = [a for a in second['claude']['accounts'] if a['name'] == args.account][0]
        check('second_poll', 'bytes_read_last_poll', a2['bytes_read_last_poll'], 0)
        check('second_poll', 'window_cost_unchanged', a2['window']['est_cost_nano'], acct['window']['est_cost_nano'])
    write_records(args.records_out, out)
    print('status compare: %d fields, %d mismatches, %d models, %d days, tier %s'
          % (len(out), bad, len(models), len(daily), acct['tier']))
    return 1 if bad else 0


def tamper_status(args):
    status = json.load(open(args.infile, encoding='utf-8'))
    status['claude']['accounts'][0]['window']['est_cost_nano'] += 1
    json.dump(status, open(args.outfile, 'w', encoding='utf-8'))
    print('tampered the first account window cost of %s' % args.infile)
    return 0


def selftest(args):
    catalog, q1 = parse_page(open(args.page, encoding='utf-8').read())
    checks = [
        ('claude-opus-5-5', 'input_nano', 4000), ('claude-opus-5-5', 'write5m_nano', 5000),
        ('claude-opus-5-5', 'write1h_nano', 8000), ('claude-opus-5-5', 'read_nano', 200),
        ('claude-opus-5-5', 'output_nano', 20000), ('claude-opus-5-5', 'fast_input_nano', 8000),
        ('claude-opus-5-5', 'fast_output_nano', 40000), ('claude-opus-5-5', 'fast_write5m_nano', 10000),
        ('claude-opus-5-5', 'fast_read_nano', 400),
        ('claude-opus-5', 'fast_input_nano', 10000), ('claude-opus-4-8', 'fast_output_nano', 50000),
        ('claude-sonnet-5', 'input_nano', 2000), ('claude-sonnet-5', 'output_nano', 10000),
        ('claude-sonnet-5-5', 'read_nano', 200),
        ('claude-haiku-4-5', 'read_nano', 100), ('claude-fable-5-1', 'read_nano', 250),
        ('claude-haiku-3-5', 'input_nano', 800),
    ]
    failed = 0
    for model, field, want in checks:
        got = catalog.get(model, {}).get(field)
        if got != want:
            print('SELFTEST FAIL %s %s: got %r want %r' % (model, field, got, want))
            failed += 1
    if q1 or 'claude-haiku-5-5' in catalog:
        print('SELFTEST FAIL first page should have no quarantine and no haiku 5.5: %r' % q1)
        failed += 1
    n = len(checks) + 1
    if args.page2:
        c2, q2 = parse_page(open(args.page2, encoding='utf-8').read())
        checks2 = [
            ('claude-sonnet-5-5', 'read_nano', 100), ('claude-opus-5-5', 'read_nano', 200),
            ('claude-haiku-5-5', 'input_nano', 100), ('claude-haiku-5-5', 'write5m_nano', 125),
            ('claude-haiku-5-5', 'write1h_nano', 200), ('claude-haiku-5-5', 'read_nano', 10),
            ('claude-haiku-5-5', 'output_nano', 500), ('claude-haiku-5-5', 'tier_threshold_tokens', 100000),
            ('claude-haiku-5-5', 'upper_input_nano', 500), ('claude-haiku-5-5', 'upper_write5m_nano', 625),
            ('claude-haiku-5-5', 'upper_write1h_nano', 1000), ('claude-haiku-5-5', 'upper_read_nano', 50),
            ('claude-haiku-5-5', 'upper_output_nano', 2500),
        ]
        for model, field, want in checks2:
            got = c2.get(model, {}).get(field)
            if got != want:
                print('SELFTEST FAIL page2 %s %s: got %r want %r' % (model, field, got, want))
                failed += 1
        if q2 or 'fast_input_nano' in c2.get('claude-haiku-5-5', {}):
            print('SELFTEST FAIL page2 quarantine/fast expectations: %r' % q2)
            failed += 1
        n += len(checks2) + 1
    resolves = [('claude-haiku-4-5-20251001', 'claude-haiku-4-5'), ('claude-opus-5-5', 'claude-opus-5-5'),
                ('claude-opus-5', 'claude-opus-5'), ('claude-opus-5-6', None),
                ('claude-opus-5-20260101', 'claude-opus-5'), ('claude-opus-5-5-20260101', 'claude-opus-5-5'),
                ('claude-opus-5-5x', None), ('claude-unknown-9', None)]
    for model, want in resolves:
        got = resolve(catalog, model)
        if got != want:
            print('SELFTEST FAIL resolve %s: got %r want %r' % (model, got, want))
            failed += 1
    print('selftest: %d checks, %d failed, %d models' % (n + len(resolves), failed, len(catalog)))
    return 1 if failed else 0


def main(argv):
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('selftest'); s.add_argument('--page', required=True); s.add_argument('--page2')
    s = sub.add_parser('prices'); s.add_argument('--page', required=True)
    s = sub.add_parser('usage'); s.add_argument('--root', required=True)
    s.add_argument('--page'); s.add_argument('--versions')
    s = sub.add_parser('tamper'); s.add_argument('--in', dest='infile', required=True)
    s.add_argument('--out', dest='outfile', required=True)
    s = sub.add_parser('status-compare'); s.add_argument('--status', required=True)
    s.add_argument('--second'); s.add_argument('--account', required=True); s.add_argument('--root', required=True)
    s.add_argument('--page', required=True); s.add_argument('--creds', required=True); s.add_argument('--records-out')
    s = sub.add_parser('tamper-status'); s.add_argument('--in', dest='infile', required=True)
    s.add_argument('--out', dest='outfile', required=True)
    s = sub.add_parser('compare')
    s.add_argument('--kind', choices=['prices', 'usage'], required=True)
    s.add_argument('--cpp', required=True); s.add_argument('--page'); s.add_argument('--versions')
    s.add_argument('--root'); s.add_argument('--records-out')
    args = ap.parse_args(argv)
    try:
        if args.cmd == 'selftest':
            return selftest(args)
        if args.cmd == 'tamper':
            return tamper(args)
        if args.cmd == 'status-compare':
            return status_compare(args)
        if args.cmd == 'tamper-status':
            return tamper_status(args)
        if args.cmd == 'prices':
            catalog, quarantined = parse_page(open(args.page, encoding='utf-8').read())
            print(json.dumps({'models': catalog, 'quarantined': quarantined}, indent=1, sort_keys=True))
            return 0
        if args.cmd == 'usage':
            rows, skipped = collect_usage(args.root, load_versions(args))
            for r in rows:
                print(json.dumps(r, sort_keys=True))
            print('skipped lines: %d' % skipped, file=sys.stderr)
            return 0
        if args.kind == 'prices':
            if not args.page:
                raise OracleError('--page is required for prices compare')
            return compare_prices(args)
        if not args.root or not (args.page or args.versions):
            raise OracleError('--root and --page or --versions are required for usage compare')
        return compare_usage(args)
    except OracleError as exc:
        print('oracle error: %s' % exc, file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
