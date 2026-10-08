#!/usr/bin/env bash
#
# Shows which swapchains macOS presents "Direct" (no compositor pass). Runs
# tests/d3d11_present.c fullscreen with the Metal HUD, one variant after the other,
# 8 seconds each; the variant name is the last line of the HUD's neutron block.
# Read "Direct" or "Composited" in the HUD's top right for each and note them.
# Do not take screenshots meanwhile (that switches to Composited).
#
# Usage: dev/direct-test.sh [variant...]   (default: all)
# Env: NEUTRON_FILES (runtime, default: the installed tool's files),
#      BENCH_CC (x86_64 mingw clang, default: llvm-mingw from ./build.sh)
#   bgra8         8-bit backbuffer, the normal case (Unity games)
#   rgb10a2       10-bit backbuffer (Unreal default), layer BGR10A2 (neutron default)
#   rgb10a2-keep  10-bit backbuffer, layer RGB10A2 like DXMT upstream
#   rgb10a2-8bit  10-bit backbuffer, layer BGRA8 (NEUTRON_LAYER_FORMAT=bgra8)
#   small         8-bit backbuffer at 70% of the window, drawable at screen size (neutron)
#   small-old     the same with DXMT's own drawable size (NEUTRON_NATIVE_DRAWABLE=0)
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NB="$REPO/.native-build"
WORK="$NB/opt/present"
FILES="${NEUTRON_FILES:-$HOME/Library/Application Support/neutron/compatibilitytools.d/neutron_proton/files}"
EXE="$WORK/d3d11_present.exe"

CC="${BENCH_CC:-}"
if [ -z "$CC" ]; then
  for c in "$NB"/toolchains/llvm-mingw-*/bin/x86_64-w64-mingw32-clang; do CC="$c"; done
fi
[ -x "$CC" ] || { echo "no x86_64-w64-mingw32-clang (run ./build.sh or set BENCH_CC)" >&2; exit 1; }
[ -x "$FILES/bin/wine" ] || { echo "no neutron runtime at $FILES (run ./install.sh or set NEUTRON_FILES)" >&2; exit 1; }

mkdir -p "$WORK" "$NB/opt/presentdata"
if [ ! -f "$EXE" ] || [ "$REPO/tests/d3d11_present.c" -nt "$EXE" ]; then
  "$CC" -O2 -o "$EXE" "$REPO/tests/d3d11_present.c" -ld3d11 -ldxgi -luser32
fi

run() {  # run <name> <format> <scale%> [VAR=value...]
  local name="$1" format="$2" scale="$3"; shift 3
  printf '%-14s %s\n' "$name" "(8 s)"
  env SteamAppId=present STEAM_COMPAT_DATA_PATH="$NB/opt/presentdata" \
    NEUTRON_FILES="$FILES" NEUTRON_HUD=1 NEUTRON_HUD_NOTE="Test $name" "$@" \
    "$REPO/tool/neutron" runinprefix "$EXE" "$format" 8 "$scale" > /dev/null 2>&1 || echo "  failed"
}

variants=("$@")
[ ${#variants[@]} -gt 0 ] || variants=(bgra8 rgb10a2 rgb10a2-keep rgb10a2-8bit small small-old)
for v in "${variants[@]}"; do
  case "$v" in
    bgra8)        run bgra8 bgra8 100 ;;
    rgb10a2)      run rgb10a2 rgb10a2 100 ;;
    rgb10a2-keep) run rgb10a2-keep rgb10a2 100 NEUTRON_LAYER_FORMAT=keep ;;
    rgb10a2-8bit) run rgb10a2-8bit rgb10a2 100 NEUTRON_LAYER_FORMAT=bgra8 ;;
    small)        run small bgra8 70 ;;
    small-old)    run small-old bgra8 70 NEUTRON_NATIVE_DRAWABLE=0 ;;
    *) echo "unknown variant $v" >&2; exit 2 ;;
  esac
done
