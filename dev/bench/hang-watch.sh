#!/bin/bash
# usage: watch.sh <appid> <label> [maxsecs]; starts game, watchdog on stalled fps, kills prefix. prints RESULT line
set -uo pipefail
APP="$1"; LABEL="$2"; MAX="${3:-90}"
ROOT=/Users/lixsl/Repositorys/lixsl/neutron
CD="$HOME/Library/Application Support/Steam/steamapps/compatdata/$APP"
case "$APP" in
 427410) DIR="AbioticFactor"; EXE="AbioticFactor.exe"; ARGS="-dx11";;
 3892270) DIR="Gamble With Your Friends"; EXE="Gamble With Your Friends.exe"; ARGS="-force-d3d11";;
esac
export STEAM_COMPAT_DATA_PATH="$CD" STEAM_COMPAT_CLIENT_INSTALL_PATH="$HOME/Library/Application Support/Steam" STEAM_COMPAT_APP_ID="$APP" NEUTRON_FILES="$ROOT/.native-build/wine-install" NEUTRON_FPS_LOG=1
unset NEUTRON_HUD MTL_HUD_ENABLED
[ "$APP" = 427410 ] && export NEUTRON_FEX_TSO="${NEUTRON_FEX_TSO:-strict}"
LOG="$HOME/Library/Logs/neutron/neutron-$APP.log"
: > "$LOG" 2>/dev/null || true
cd "$HOME/Library/Application Support/Steam/steamapps/common/$DIR"
"$ROOT/tool/neutron" waitforexitandrun "$PWD/$EXE" $ARGS >/dev/null 2>&1 &
start=$(date +%s); n=0; last=$start; dumped=0
while :; do
  sleep 2; now=$(date +%s)
  c=$(grep -c 'neutron: fps' "$LOG" 2>/dev/null || true); c=${c:-0}
  if [ "$c" -ne "$n" ]; then n=$c; last=$now; fi
  if [ "$n" -ge 1 ] && [ $((now-last)) -ge 20 ] && [ "$DUMP" = 1 ] && [ $dumped = 0 ]; then
    dumped=1; TS=$(date +%Y%m%d-%H%M%S); OUT="$ROOT/.native-build/opt/hang/$TS-$LABEL"; mkdir -p "$OUT"
    pid=$(pgrep -f "^Z:.*(Win64-Shipping|Gamble)" | tail -1); ws=$(pgrep -f "bin/wineserver" | head -1)
    echo "pid=$pid ws=$ws" > "$OUT/pids"
    [ -n "$pid" ] && sample "$pid" 5 -file "$OUT/game.sample" >/dev/null 2>&1
    [ -n "$ws" ] && sample "$ws" 3 -file "$OUT/ws.sample" >/dev/null 2>&1
    [ -n "$pid" ] && vmmap -summary "$pid" > "$OUT/vmmap.txt" 2>&1
    [ -n "$pid" ] && perl -e 'alarm 60; exec @ARGV' lldb -p "$pid" --batch -o "thread backtrace all" > "$OUT/lldb.txt" 2>&1
    cp "$LOG" "$OUT/neutron.log" 2>/dev/null
  fi
  if [ $((now-start)) -ge "$MAX" ] || { [ "$n" -ge 1 ] && [ $((now-last)) -ge 30 ]; }; then break; fi
  [ "$n" -ge 12 ] && break
done
echo "RESULT $LABEL fpslines=$n elapsed=$((now-start)) lastgap=$((now-last))"
WINEPREFIX="$CD/pfx" "$ROOT/.native-build/wine-install/bin/wineserver" -k 2>/dev/null
sleep 3
wait 2>/dev/null
