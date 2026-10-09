#!/bin/bash
#
# neutron benchmark suite. Runs everything through tool/neutron (same environment,
# FEX registration and TSO default as a Steam start) and prints one JSON object:
#   {"meta": {...}, "metrics": {"<group>.<name>_<unit>": median, ...}, "spread_pct": {...}}
# and appends it to .native-build/opt/bench-history.jsonl.
#
#   cpu.*         dev/bench/cpu.c, K runs, ns per op (FEX fast TSO, the tool default)
#   cpu_strict.*  the TSO-sensitive cpu tests again with NEUTRON_FEX_TSO=strict
#   start.*       wall time of starting a no-op exe through the tool (warm wineserver)
#   d3d11_5k.*    dev/bench/d3d11.c, 5000 draws per frame: draw loop time and process CPU
#                 time per frame (its FPS sits at the display refresh, so it is left out)
#   d3d11_20k.*   20000 draws per frame, CPU bound, so its fps is a CPU number too
#   gwyfs.*       with --games: Gamble again with forced strict TSO (NEUTRON_FEX_TSO=strict)
#                 and NEUTRON_BENCH_NOVSYNC=1 (DXMT draws the present pass offscreen and
#                 never presents). Strict alone still sits at the 160 Hz cap in the menu;
#                 without the present, fps and wall_ms_per_frame are not capped and show
#                 gains that gwyf hides
#   gwyf.*, af.*  with --games: Gamble With Your Friends and Abiotic Factor main menu,
#                 FPS log (one line per second of rendering), 20 lines warmup, median
#                 over the next 40; first_fps_s is the time from launch to the first
#                 FPS line, menu_s to steady rendering (after loading stalls),
#                 cpu_per_frame_ms the CPU time of the game process per frame (shows
#                 gains when a menu sits at the display refresh), busy_cores the same
#                 CPU time over wall time, wall_ms_per_frame is 1000 / median fps
#
# Usage: dev/bench/run.sh [--games] [--only-games] [--game gwyf|gwyfs|af] [-k N] [--out FILE]
#                         [--compare BASELINE]
#   --games     micro benches and both games, --only-games skips the micro benches,
#               --game X runs only that game (and no micro benches)
#   -k N        runs per micro bench (default 5, median is taken)
#   --compare   prints the change of every metric against a baseline from bench.py baseline
# Env: NEUTRON_FILES (runtime, default: the installed tool's files), BENCH_CC (x86_64 mingw clang).
# Games are skipped when one of them already runs (another session); the result says so.
# Make a baseline from full runs: dev/bench/bench.py baseline run1.json run2.json > baseline.json
#
set -euo pipefail

BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$BENCH_DIR/../.." && pwd)"
NB="$REPO/.native-build"
OPT="$NB/opt"
WORK="$OPT/bench"
TOOL="$REPO/tool/neutron"
PY="$BENCH_DIR/bench.py"
FILES="${NEUTRON_FILES:-$HOME/Library/Application Support/neutron/compatibilitytools.d/neutron_proton/files}"
STEAM_ROOT="$HOME/Library/Application Support/Steam"
HISTORY="$OPT/bench-history.jsonl"

K=5
GAMES=0
GAME_LIST="gwyf gwyfs af"
MICRO=1
OUT=""
COMPARE=""
while [ $# -gt 0 ]; do
  case "$1" in
    --games) GAMES=1 ;;
    --only-games) GAMES=1; MICRO=0 ;;
    --game) GAMES=1; MICRO=0; GAME_LIST="$2"; shift ;;
    -k) K="$2"; shift ;;
    --out) OUT="$2"; shift ;;
    --compare) COMPARE="$2"; shift ;;
    -h|--help) sed -n '2,/^set -euo/p' "$0" | sed '$d'; exit 0 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done

mkdir -p "$WORK"
RAW="$WORK/raw.jsonl"
: > "$RAW"
say() { printf '[bench] %s\n' "$*" >&2; }
now_ms() { perl -MTime::HiRes=time -e 'printf "%.0f\n", time * 1000'; }

#-------------------------------------------------------------------------------
#  build
#-------------------------------------------------------------------------------

CC="${BENCH_CC:-}"
if [ -z "$CC" ]; then
  for c in "$NB"/toolchains/llvm-mingw-*/bin/x86_64-w64-mingw32-clang; do CC="$c"; done
fi
[ -x "$CC" ] || { echo "no x86_64-w64-mingw32-clang (run ./build.sh or set BENCH_CC)" >&2; exit 1; }
[ -x "$FILES/bin/wine" ] || { echo "no neutron runtime at $FILES (run ./install.sh or set NEUTRON_FILES)" >&2; exit 1; }

build() {
  local src="$1" exe="$2"; shift 2
  if [ ! -f "$exe" ] || [ "$src" -nt "$exe" ]; then
    say "building $(basename "$exe")"
    "$CC" -O2 -Wall -o "$exe" "$src" "$@"
  fi
}
build "$BENCH_DIR/cpu.c" "$WORK/cpu.exe"
build "$BENCH_DIR/d3d11.c" "$WORK/d3d11.exe" -ld3d11 -ld3dcompiler -lgdi32 -luser32

#-------------------------------------------------------------------------------
#  meta
#-------------------------------------------------------------------------------

# The HUD (NEUTRON_HUD, MTL_HUD_ENABLED) costs frame time, so benchmarks run without it.
# BENCH_KEEP_HUD=1 keeps it (only to measure the HUD itself). The removed values are recorded.
HUD_REMOVED=""
if [ "${BENCH_KEEP_HUD:-0}" != 1 ]; then
  for v in NEUTRON_HUD MTL_HUD_ENABLED MTL_HUD_SCALE; do
    if [ -n "${!v:-}" ]; then HUD_REMOVED="$HUD_REMOVED $v=${!v}"; unset "$v"; fi
  done
fi
export HUD_REMOVED

other_wine="$( (pgrep -f 'wineserver|\.exe' 2>/dev/null || true) | wc -l | tr -d ' ')"
git_rev="$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo none)"
git -C "$REPO" diff --quiet HEAD 2>/dev/null || git_rev="$git_rev+dirty"
runtime_stamp="$(stat -f '%Sm' -t '%F %T' "$FILES/lib/wine/aarch64-unix/ntdll.so" 2>/dev/null || echo none)"
load="$(sysctl -n vm.loadavg | awk '{print $2}')"
GAME_RE='Gamble With Your Friends.exe|AbioticFactor'
games_running() { (pgrep -f "$GAME_RE" 2>/dev/null || true) | wc -l | tr -d ' '; }
printf '{"meta":{"time":"%s","git":"%s","runtime":"%s","ntdll_mtime":"%s","k":%s,"load1_start":%s,"wine_procs_start":%s,"game_procs_start":%s}}\n' \
  "$(date '+%F %T')" "$git_rev" "$FILES" "$runtime_stamp" "$K" "$load" "$other_wine" "$(games_running)" >> "$RAW"
# runtime knobs set by the caller (they change the numbers, so they go into the result)
python3 -c 'import json, os, re; print(json.dumps({"meta": {"env": {k: v for k, v in sorted(os.environ.items())
  if re.match(r"(NEUTRON_|FEX_|DXMT_|WINE|MTL_)", k) and k != "NEUTRON_FILES"}, "hud_unset": os.environ.get("HUD_REMOVED", "").split()}}))' >> "$RAW"

#-------------------------------------------------------------------------------
#  micro benches: own prefix through the tool (registers FEX like for a game)
#-------------------------------------------------------------------------------

BENCH_DATA="$OPT/benchdata"
# runs a Windows exe in the bench prefix through the tool; extra env as leading VAR=x args
in_prefix() {
  env SteamAppId=bench STEAM_COMPAT_DATA_PATH="$BENCH_DATA" NEUTRON_FILES="$FILES" "$@"
}

if [ "$MICRO" = 1 ]; then
  mkdir -p "$BENCH_DATA"
  say "prefix setup"
  in_prefix "$TOOL" runinprefix "$WORK/cpu.exe" --nop > /dev/null 2>&1

  for i in $(seq "$K"); do
    say "cpu run $i/$K"
    in_prefix "$TOOL" runinprefix "$WORK/cpu.exe" 2>/dev/null | python3 "$PY" wrap cpu ns >> "$RAW" || true
  done
  # strict TSO adds vector and rep movs/stos ordering: the tests that touch those
  for i in $(seq "$K"); do
    say "cpu strict TSO run $i/$K"
    in_prefix NEUTRON_FEX_TSO=strict "$TOOL" runinprefix "$WORK/cpu.exe" \
      float_sse,scalar_loadstore,memcpy,memset,rep_movsb,atomic_inc 2>/dev/null \
      | python3 "$PY" wrap cpu_strict ns >> "$RAW" || true
  done
  for i in $(seq "$K"); do
    t0="$(now_ms)"
    in_prefix "$TOOL" runinprefix "$WORK/cpu.exe" --nop > /dev/null 2>&1
    t1="$(now_ms)"
    printf '{"group":"start","values":{"nop_exe_ms":%s}}\n' "$((t1 - t0))" >> "$RAW"
  done
  # A game in front (fullscreen) makes macOS run the bench window's process 2 to 3 times
  # slower (background app), so the D3D11 numbers would be wrong: skip them then.
  for i in $(seq "$K"); do
    if [ "$(games_running)" != 0 ]; then
      say "d3d11: a game is running (other session), skipped"
      printf '{"group":"%s","skipped":"a game was running"}\n' d3d11_5k d3d11_20k >> "$RAW"
      break
    fi
    say "d3d11 run $i/$K"
    in_prefix "$TOOL" runinprefix "$WORK/d3d11.exe" 5000 300 2>/dev/null | python3 "$PY" wrap d3d11_5k "" submit_ms,proc_cpu_ms >> "$RAW" || true
    in_prefix "$TOOL" runinprefix "$WORK/d3d11.exe" 20000 200 2>/dev/null | python3 "$PY" wrap d3d11_20k "" fps,submit_ms,proc_cpu_ms >> "$RAW" || true
  done
  WINEPREFIX="$BENCH_DATA/pfx" "$FILES/bin/wineserver" -k 2>/dev/null || true
  # a game of another session running during the micro benches makes them noisy
  printf '{"meta":{"game_procs_after_micro":%s,"load1_after_micro":%s}}\n' \
    "$(games_running)" "$(sysctl -n vm.loadavg | awk '{print $2}')" >> "$RAW"
fi

#-------------------------------------------------------------------------------
#  games: main menu FPS through the tool, like Steam starts them
#-------------------------------------------------------------------------------

GAME_PFX=""
WATCH_PID=""
stop_game() {
  if [ -n "$WATCH_PID" ]; then kill "$WATCH_PID" 2>/dev/null || true; fi
  if [ -n "$GAME_PFX" ]; then
    WINEPREFIX="$GAME_PFX" "$FILES/bin/wineserver" -k 2>/dev/null || true
    GAME_PFX=""
  fi
}
trap stop_game EXIT
trap 'exit 130' INT TERM

# game <group> <appid> <dir> <exe> <first FPS timeout s> <process regex> <args...>
game() {
  local group="$1" appid="$2" dir="$3" exe="$4" first_timeout="$5" proc="$6"; shift 6
  local game_dir="$STEAM_ROOT/steamapps/common/$dir"
  local compat="$STEAM_ROOT/steamapps/compatdata/$appid"
  local log="$HOME/Library/Logs/neutron/neutron-$appid.log"
  if [ "$(games_running)" != 0 ]; then
    say "$group: a game is already running (other session), skipped"
    printf '{"group":"%s","skipped":"another game was running"}\n' "$group" >> "$RAW"
    return 0
  fi
  [ -f "$game_dir/$exe" ] || { printf '{"group":"%s","skipped":"not installed"}\n' "$group" >> "$RAW"; return 0; }
  local offset=0
  [ -f "$log" ] && offset="$(stat -f %z "$log")"
  say "$group: starting $exe"
  GAME_PFX="$compat/pfx"
  (
    cd "$game_dir"
    export STEAM_COMPAT_DATA_PATH="$compat" STEAM_COMPAT_CLIENT_INSTALL_PATH="$STEAM_ROOT" \
      STEAM_COMPAT_APP_ID="$appid" NEUTRON_FILES="$FILES" NEUTRON_FPS_LOG=1
    # GAME_ENV: extra "VAR=value" words for this profile (gwyfs: strict TSO)
    # shellcheck disable=SC2086
    # GAME_ENV is a list of VAR=value words
    # shellcheck disable=SC2086,SC2163
    [ -z "${GAME_ENV:-}" ] || export $GAME_ENV
    exec "$TOOL" waitforexitandrun "$game_dir/$exe" "$@"
  ) > /dev/null 2>&1 &
  local pid=$!
  # in the background plus wait, so INT/TERM reach the trap (which stops the game) right away
  python3 "$PY" watch "$log" "$offset" "$pid" 20 40 "$first_timeout" "$proc" > "$WORK/watch.json" &
  WATCH_PID=$!
  wait "$WATCH_PID" || true
  WATCH_PID=""
  python3 "$PY" wrap "$group" < "$WORK/watch.json" >> "$RAW" || true
  say "$group: stopping"
  stop_game
  wait "$pid" 2>/dev/null || true
  sleep 3
}

if [ "$GAMES" = 1 ] && [[ " $GAME_LIST " == *" gwyf "* ]]; then
  game gwyf 3892270 "Gamble With Your Friends" "Gamble With Your Friends.exe" 180 \
    '^Z:.*Gamble With Your Friends\.exe' -force-d3d11
fi
if [ "$GAMES" = 1 ] && [[ " $GAME_LIST " == *" gwyfs "* ]]; then
  GAME_ENV="NEUTRON_FEX_TSO=strict NEUTRON_BENCH_NOVSYNC=1" game gwyfs 3892270 "Gamble With Your Friends" "Gamble With Your Friends.exe" 180 \
    '^Z:.*Gamble With Your Friends\.exe' -force-d3d11
fi
if [ "$GAMES" = 1 ] && [[ " $GAME_LIST " == *" af "* ]]; then
  game af 427410 AbioticFactor AbioticFactor.exe 300 'AbioticFactor-Win64-Shipping\.exe' -dx11
fi

#-------------------------------------------------------------------------------
#  result
#-------------------------------------------------------------------------------

RESULT="$(python3 "$PY" summarize "$RAW")"
mkdir -p "$(dirname "$HISTORY")"
case "$RESULT" in *'"metrics": {}'*) ;; *) printf '%s\n' "$RESULT" >> "$HISTORY" ;; esac
if [ -n "$OUT" ]; then printf '%s\n' "$RESULT" > "$OUT"; fi
printf '%s\n' "$RESULT"
if [ -n "$COMPARE" ]; then
  tmp="$WORK/last.json"
  printf '%s\n' "$RESULT" > "$tmp"
  python3 "$PY" compare "$COMPARE" "$tmp" >&2
fi
