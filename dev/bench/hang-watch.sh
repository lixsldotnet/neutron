#!/usr/bin/env bash
#
# Starts a game through tool/neutron and watches its FPS log for stalls. When the
# game rendered once and then logs no FPS line for 20 s, it dumps the game and the
# wineserver once (sample, vmmap, lldb backtraces of all threads, the game log).
# Ends after the given time, after a 30 s stall or after 12 FPS lines, kills the
# Wine session of the prefix and prints one RESULT line.
#
# Usage: dev/bench/hang-watch.sh <appid> <label> [max seconds, default 90]
#   appid: 427410 (Abiotic Factor) or 3892270 (Gamble With Your Friends), installed
#          in the default Steam library
# Env: NEUTRON_FILES (runtime, default: the installed tool's files),
#      HANG_DUMP=0 (no dumps), HANG_OUT (dump folder, default .native-build/opt/hang)
# lldb asks for developer tools access the first time.
#
set -euo pipefail

[ $# -ge 2 ] || { sed -n '2,/^set -euo/p' "$0" | sed '$d'; exit 2; }
APP="$1" LABEL="$2" MAX="${3:-90}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
STEAM_ROOT="$HOME/Library/Application Support/Steam"
FILES="${NEUTRON_FILES:-$HOME/Library/Application Support/neutron/compatibilitytools.d/neutron_proton/files}"
DUMP="${HANG_DUMP:-1}"
HANG_OUT="${HANG_OUT:-$ROOT/.native-build/opt/hang}"
CD="$STEAM_ROOT/steamapps/compatdata/$APP"
LOG="$HOME/Library/Logs/neutron/neutron-$APP.log"

case "$APP" in
  427410)  DIR="AbioticFactor"; EXE="AbioticFactor.exe"; PROC='AbioticFactor-Win64-Shipping\.exe'
           ARGS=(-dx11) ;;
  3892270) DIR="Gamble With Your Friends"; EXE="Gamble With Your Friends.exe"; PROC='^Z:.*Gamble With Your Friends\.exe'
           ARGS=(-force-d3d11) ;;
  *) echo "unknown appid $APP (known: 427410, 3892270)" >&2; exit 2 ;;
esac
GAME_DIR="$STEAM_ROOT/steamapps/common/$DIR"
[ -f "$GAME_DIR/$EXE" ] || { echo "not installed: $GAME_DIR/$EXE" >&2; exit 1; }
[ -x "$FILES/bin/wineserver" ] || { echo "no neutron runtime at $FILES" >&2; exit 1; }

export STEAM_COMPAT_DATA_PATH="$CD" STEAM_COMPAT_CLIENT_INSTALL_PATH="$STEAM_ROOT" \
  STEAM_COMPAT_APP_ID="$APP" NEUTRON_FILES="$FILES" NEUTRON_FPS_LOG=1
unset NEUTRON_HUD MTL_HUD_ENABLED
# Abiotic Factor needs strict TSO (tool/neutron APP_TSO_427410), keep it unless set.
[ "$APP" = 427410 ] && export NEUTRON_FEX_TSO="${NEUTRON_FEX_TSO:-strict}"

mkdir -p "$(dirname "$LOG")"
: > "$LOG"
(cd "$GAME_DIR" && exec "$ROOT/tool/neutron" waitforexitandrun "$GAME_DIR/$EXE" "${ARGS[@]}") >/dev/null 2>&1 &

dump() {
  local out pid ws
  out="$HANG_OUT/$(date +%Y%m%d-%H%M%S)-$LABEL"
  mkdir -p "$out"
  pid="$( (pgrep -f "$PROC" || true) | tail -1)"
  ws="$( (pgrep -f "bin/wineserver" || true) | head -1)"
  echo "pid=$pid ws=$ws" > "$out/pids"
  if [ -n "$pid" ]; then
    sample "$pid" 5 -file "$out/game.sample" >/dev/null 2>&1 || true
    vmmap -summary "$pid" > "$out/vmmap.txt" 2>&1 || true
    perl -e 'alarm 60; exec @ARGV' lldb -p "$pid" --batch -o "thread backtrace all" > "$out/lldb.txt" 2>&1 || true
  fi
  [ -z "$ws" ] || sample "$ws" 3 -file "$out/ws.sample" >/dev/null 2>&1 || true
  cp "$LOG" "$out/neutron.log" 2>/dev/null || true
  echo "dump: $out"
}

start=$(date +%s) n=0 last=$start dumped=0 now=$start
while :; do
  sleep 2
  now=$(date +%s)
  c=$(grep -c 'neutron: fps' "$LOG" 2>/dev/null || true)
  c=${c:-0}
  if [ "$c" -ne "$n" ]; then n=$c; last=$now; fi
  if [ "$n" -ge 1 ] && [ $((now - last)) -ge 20 ] && [ "$DUMP" = 1 ] && [ "$dumped" = 0 ]; then
    dumped=1
    dump
  fi
  if [ $((now - start)) -ge "$MAX" ] || { [ "$n" -ge 1 ] && [ $((now - last)) -ge 30 ]; }; then break; fi
  [ "$n" -ge 12 ] && break
done
echo "RESULT $LABEL fpslines=$n elapsed=$((now - start)) lastgap=$((now - last))"
WINEPREFIX="$CD/pfx" "$FILES/bin/wineserver" -k 2>/dev/null || true
sleep 3
wait 2>/dev/null || true
