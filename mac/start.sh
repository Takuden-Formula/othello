#!/bin/bash
# Othello - macOS launcher. Called from 起動_Mac.command / 起動_全画面_Mac.command (serves docs/)
# Usage: bash mac/start.sh [--kiosk]
cd "$(dirname "$0")/.." || exit 1
ROOT="$(pwd)"
LOG="$ROOT/server_log.txt"
log() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; }

PORTFILE="$(mktemp "${TMPDIR:-/tmp}/othello_port.XXXXXX")"
rm -f "$PORTFILE"

# start the server (Perl is always included in macOS; Python is used only if Perl is missing)
if command -v perl >/dev/null 2>&1; then
  perl "$ROOT/mac/serve.pl" "$ROOT/docs" "$PORTFILE" >>"$LOG" 2>&1 &
elif command -v python3 >/dev/null 2>&1 && python3 -c 'import http.server' >/dev/null 2>&1; then
  python3 - "$ROOT/docs" "$PORTFILE" >>"$LOG" 2>&1 <<'PY' &
import sys, os, functools, http.server, socketserver
root, pf = sys.argv[1], sys.argv[2]
http.server.SimpleHTTPRequestHandler.extensions_map.update({'.mjs': 'text/javascript', '.wasm': 'application/wasm', '.task': 'application/octet-stream'})
H = functools.partial(http.server.SimpleHTTPRequestHandler, directory=root)
for p in range(8801, 8810):
    try: s = socketserver.ThreadingTCPServer(('127.0.0.1', p), H); break
    except OSError: s = None
if not s: sys.exit('ERROR: no free port in 8801-8809')
open(pf, 'w').write(str(p)); print('PORT', p, flush=True); s.serve_forever()
PY
else
  log 'ERROR: Perl も Python も見つかりません'; read -r -p 'Enter で閉じます'; exit 1
fi
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null; rm -f "$PORTFILE"' EXIT INT TERM HUP

for _ in $(seq 1 50); do [ -s "$PORTFILE" ] && break; kill -0 $SERVER_PID 2>/dev/null || break; sleep 0.1; done
PORT="$(cat "$PORTFILE" 2>/dev/null)"
if [ -z "$PORT" ]; then log 'ERROR: サーバを起動できません（server_log.txt を見てください）'; read -r -p 'Enter で閉じます'; exit 1; fi
URL="http://localhost:$PORT/index.html"
log "Serving $ROOT at $URL"

# open Chrome (or Edge, or the default browser = Safari)
app_path() { for d in "/Applications" "$HOME/Applications"; do [ -d "$d/$1.app" ] && { echo "$d/$1.app"; return 0; }; done; return 1; }
BROWSER=""
for name in "Google Chrome" "Microsoft Edge" "Chromium"; do
  if app_path "$name" >/dev/null; then BROWSER="$name"; break; fi
done
FLAGS=(--autoplay-policy=no-user-gesture-required)
if [ "$1" = "--kiosk" ]; then URL="$URL?fs=1"; FLAGS+=("--app=$URL" --start-maximized); else FLAGS+=(--new-window "$URL"); fi
if [ -n "$BROWSER" ]; then
  open -na "$(app_path "$BROWSER")" --args "${FLAGS[@]}"; log "Browser: $BROWSER"
else
  open "$URL"
  log 'Chrome / Edge が見つからないので標準のブラウザ（Safari）で開きました。'
  log '遊ぶことはできますが、記録フォルダ（records.csv）への保存は Chrome / Edge だけの機能です（Safari ではブラウザ内に残ります）。'
fi

echo
echo 'この窓を閉じる（または Ctrl+C）とサーバが止まります。'
wait $SERVER_PID
