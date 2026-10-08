#!/bin/bash
# Gate g6 live run: a throwaway aimon daemon (isolated HOME, own port, loopback
# only, other collectors and the supervisor disabled) collects from a frozen
# copy of the real transcripts. It is started from a directory that has no web/
# folder, so the dashboard is served from the assets embedded in the binary.
# The script then calls the MCP tool, loads the dashboard in headless Chromium,
# saves the status document, the rendered DOM and a screenshot, and stops the
# daemon. Run from the repository root.
#
# Copyright (C) 2026, Charles Chiou
set -euo pipefail

REAL_HOME="$HOME"
REAL_CFG="$REAL_HOME/.config/aimon"
ROOT="$(pwd)"
TH="build/throwaway-home"
CL="build/throwaway-claude"
OUT="build/g6"
PORT=3893
PAGE="test/fixtures/pricing_page_v2.md"
# Chromium cannot load any HTTP page in this environment (the browser's network stack
# hangs even for a trivial local server), so the served files are rendered from disk.
# The headless shell is used because it starts reliably here.
CHROME="$REAL_HOME/.cache/ms-playwright/chromium_headless_shell-1200/chrome-headless-shell-linux64/chrome-headless-shell"
SHOTS="plan/plan_claude_usage_collector.artifacts/screenshots"

rm -rf "$TH" "$CL" "$OUT"
mkdir -p "$TH/.config/aimon" "$CL" "$OUT" "$SHOTS"
cp -a build/recount-snapshot "$CL/projects"
ln -s "$REAL_HOME/.claude/.credentials.json" "$CL/.credentials.json"

live_fingerprint() {
    stat -c '%Y %s config.json' "$REAL_CFG/config.json"
    (cd "$REAL_CFG" && find . -maxdepth 3 -printf '%P\n' | sort)
}
BEFORE="$(live_fingerprint)"

./build/test_price_catalog --seed-cache "$TH/.config/aimon/claude_prices.json" --page "$PAGE" --now "$(date +%s)"

cat > "$TH/.config/aimon/config.json" <<JSON
{
  "antigravity": {"auto_discover": false},
  "cursor": {"auto_discover": false, "access_token": "", "db_path": "$ROOT/$TH/none.vscdb"},
  "history": {"enabled": true, "db_path": "$ROOT/$TH/.config/aimon/history.db"},
  "mqtt": {"enabled": false},
  "supervisor": {"enabled": false, "auto_restart": false},
  "web": {"host": "127.0.0.1", "port": $PORT, "endpoints_enabled": true},
  "claude": {
    "enabled": true,
    "pricing_url": "https://127.0.0.1:9/never-fetched.md",
    "accounts": [{"name": "real", "config_dir": "$ROOT/$CL", "spend_limit_usd": 500, "cycle_reset_day": 1}]
  }
}
JSON

# start from a directory without web/ so the embedded assets are served
( cd "$TH" && [ ! -e web ] && HOME="$ROOT/$TH" "$ROOT/build/aimon" daemon --config "$ROOT/$TH/.config/aimon/config.json" \
      --port "$PORT" --host 127.0.0.1 --gateway-disable --no-ncurses > "$ROOT/$OUT/daemon.log" 2>&1 ) &
PID=$!
trap 'pkill -INT -f "aimon daemon --config $ROOT/$TH" 2>/dev/null || true' EXIT

for _ in $(seq 1 100); do
    if curl -sS -m 2 -o "$OUT/status.json" "http://127.0.0.1:$PORT/api/status" 2>/dev/null && [ -s "$OUT/status.json" ]; then
        break
    fi
    sleep 0.2
done

python3 -I plan/plan_claude_usage_collector.artifacts/scripts/mcp_call.py 127.0.0.1 "$PORT" core check_claude_usage "$OUT/mcp.txt"
python3 -I plan/plan_claude_usage_collector.artifacts/scripts/mcp_call.py 127.0.0.1 "$PORT" core get_combined_ai_status "$OUT/combined.txt"

# 1. the daemon must serve, over HTTP, exactly the files under web/ (embedded assets, since
#    the daemon runs from a directory without web/)
mkdir -p "$OUT/site" "$OUT/api"
for pair in "style.css:style.css" "app.js:app.js" "qrcode.js:qrcode.js"; do
    name="${pair%%:*}"; url="${pair#*:}"
    curl -sS -m 10 -o "$OUT/site/$name" "http://127.0.0.1:$PORT/$url"
    if ! cmp -s "$OUT/site/$name" "web/$name"; then
        echo "the daemon served a different $name than web/$name" >&2
        exit 1
    fi
    echo "served $name is byte-identical to web/$name"
done
# the index page carries the daemon's per-session token script before </head>; without
# exactly that injection it must be byte-identical to web/index.html
curl -sS -m 10 -o "$OUT/site/index.html" "http://127.0.0.1:$PORT/"
python3 -I - "$OUT/site/index.html" web/index.html <<'PY'
import re, sys
served = open(sys.argv[1], encoding='utf-8').read()
source = open(sys.argv[2], encoding='utf-8').read()
stripped, n = re.subn(r'<script>\nwindow\.__UI_SESSION_TOKEN__ = "[0-9a-f]{32}";\n.*?</script>\n(?=</head>)', '', served, count=1, flags=re.S)
if n != 1:
    sys.exit('the served index.html has no session token injection before </head>')
if stripped != source:
    sys.exit('the served index.html differs from web/index.html beyond the session token injection')
print('served index.html equals web/index.html plus the session token injection')
PY

# 2. the daemon's real API responses, used to answer the page's fetch() calls
for ep in status sessions monitors services telemetry/overview telemetry/timeseries telemetry/activity_timeline \
          telemetry/tools telemetry/sessions approvals/pending mobile/devices; do
    curl -sS -m 10 -o "$OUT/api/api_$(echo "$ep" | tr / _).json" "http://127.0.0.1:$PORT/api/$ep" || true
done
for f in "$OUT"/api/*.json; do
    python3 -I -c "import json,sys; json.load(open(sys.argv[1]))" "$f" 2>/dev/null || rm -f "$f"
done
python3 -I plan/plan_claude_usage_collector.artifacts/scripts/g6_harness.py build --site "$OUT/site" --api "$OUT/api"

# 3. render the harness in the real browser through the DevTools protocol
timeout 150 python3 -I plan/plan_claude_usage_collector.artifacts/scripts/cdp_page.py --browser "$CHROME" \
    --url "file://$ROOT/$OUT/site/harness.html?no_sse=1" --dom "$OUT/dom.html" --png "$ROOT/$SHOTS/claude_card.png" \
    --errors "$OUT/js_errors.txt" --wait-sec 40 \
    --wait-js "document.getElementById('claude-status-dot').className.indexOf('dot-online') >= 0 && document.getElementById('claude-5h-cost').dataset.nano !== '0'" \
    | tee "$OUT/browser.log"
ERRORS="$(wc -l < "$OUT/js_errors.txt")"
echo "javascript errors in the browser: $ERRORS"
if [ "$ERRORS" != "0" ]; then
    head -5 "$OUT/js_errors.txt" >&2
    exit 1
fi

pkill -INT -f "aimon daemon --config $ROOT/$TH" || true
for _ in $(seq 1 50); do
    pgrep -f "aimon daemon --config $ROOT/$TH" >/dev/null || break
    sleep 0.2
done
if pgrep -f "aimon daemon --config $ROOT/$TH" >/dev/null; then
    echo "daemon did not stop on SIGINT" >&2
    exit 1
fi
trap - EXIT
wait "$PID" 2>/dev/null || true

AFTER="$(live_fingerprint)"
if [ "$BEFORE" != "$AFTER" ]; then
    echo "LIVE STATE CHANGED" >&2
    exit 1
fi
if ss -ltn 2>/dev/null | grep -q ":$PORT "; then
    echo "port $PORT still listening" >&2
    exit 1
fi
echo "live state unchanged; throwaway daemon stopped cleanly; outputs in $OUT"
