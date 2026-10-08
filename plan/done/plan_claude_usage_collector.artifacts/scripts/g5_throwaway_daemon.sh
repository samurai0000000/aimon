#!/bin/bash
# Gate g5 live run: start a throwaway aimon daemon (isolated HOME, own port,
# loopback only, collectors for other providers and the service supervisor
# disabled), let it collect from a frozen copy of the real transcripts using
# the real account's tier fields, poll twice, save both status documents and
# stop it. Run from the repository root.
#
# Copyright (C) 2026, Charles Chiou
set -euo pipefail

REAL_HOME="$HOME"
REAL_CFG="$REAL_HOME/.config/aimon"
TH="build/throwaway-home"
CL="build/throwaway-claude"
OUT="build/g5"
PORT=3893
PAGE="test/fixtures/pricing_page_v2.md"

rm -rf "$TH" "$CL" "$OUT"
mkdir -p "$TH/.config/aimon" "$CL" "$OUT"
cp -a build/recount-snapshot "$CL/projects"
ln -s "$REAL_HOME/.claude/.credentials.json" "$CL/.credentials.json"

# The live daemon rewrites its own databases all the time, so their mtimes prove
# nothing. What must not change: its configuration file and the set of files in
# its configuration directory (including the quarantine directory).
live_fingerprint() {
    stat -c '%Y %s config.json' "$REAL_CFG/config.json"
    (cd "$REAL_CFG" && find . -maxdepth 3 -printf '%P\n' | sort)
}
BEFORE="$(live_fingerprint)"

# price cache seeded as if fetched now from the saved page: no network is used
./build/test_price_catalog --seed-cache "$TH/.config/aimon/claude_prices.json" --page "$PAGE" --now "$(date +%s)"

cat > "$TH/.config/aimon/config.json" <<JSON
{
  "antigravity": {"auto_discover": false},
  "cursor": {"auto_discover": false, "access_token": "", "db_path": "$TH/none.vscdb"},
  "history": {"enabled": true, "db_path": "$TH/.config/aimon/history.db"},
  "mqtt": {"enabled": false},
  "supervisor": {"enabled": false, "auto_restart": false},
  "web": {"host": "127.0.0.1", "port": $PORT, "endpoints_enabled": true},
  "claude": {
    "enabled": true,
    "pricing_url": "https://127.0.0.1:9/never-fetched.md",
    "accounts": [{"name": "real", "config_dir": "$CL", "spend_limit_usd": 500, "cycle_reset_day": 1}]
  }
}
JSON

HOME="$(pwd)/$TH" ./build/aimon daemon --config "$TH/.config/aimon/config.json" --port "$PORT" \
    --host 127.0.0.1 --gateway-disable --no-ncurses > "$OUT/daemon.log" 2>&1 &
PID=$!
trap 'kill -INT "$PID" 2>/dev/null || true; wait "$PID" 2>/dev/null || true' EXIT

for _ in $(seq 1 100); do
    if curl -sS -m 2 -o "$OUT/status1.json" "http://127.0.0.1:$PORT/api/status" 2>/dev/null && [ -s "$OUT/status1.json" ]; then
        break
    fi
    sleep 0.2
done
curl -sS -m 30 -X POST -o /dev/null "http://127.0.0.1:$PORT/api/refresh"
curl -sS -m 10 -o "$OUT/status2.json" "http://127.0.0.1:$PORT/api/status"

kill -INT "$PID"
for _ in $(seq 1 50); do
    kill -0 "$PID" 2>/dev/null || break
    sleep 0.2
done
if kill -0 "$PID" 2>/dev/null; then
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
echo "live state files unchanged:"
echo "$AFTER"
if ss -ltn 2>/dev/null | grep -q ":$PORT "; then
    echo "port $PORT still listening" >&2
    exit 1
fi
echo "throwaway daemon stopped cleanly; statuses saved to $OUT"
