#!/usr/bin/env bash
#
# Runs the neutron sandbox tests headless: sbtest.exe (Windows, through FEX) and the
# native probe it starts as a child, once without and once with NEUTRON_SANDBOX=1,
# and prints expected against actual. Also d3d11_clear.exe (DXMT on Metal) and
# iotest.exe (audio, controllers) in the sandbox, no window.
#
# Usage: tests/sandbox/sbtest.sh [tool/neutron] [files dir]
#   Builds its test programs into .native-build/opt/sandbox-test (SBTEST_WORK) and uses
#   the installed runtime unless a files dir is given.
# Only touches its own canaries: ~/Documents/neutron-sandbox-canary/canary.txt,
# test files it deletes again, two TCP listeners on 127.0.0.1 and a unix socket in /tmp.
set -euo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SRC/../.." && pwd)"
TOOL="${1:-$REPO/tool/neutron}"
FILES="${2:-$HOME/Library/Application Support/neutron/compatibilitytools.d/neutron_proton/files}"
OUT="${SBTEST_WORK:-$REPO/.native-build/opt/sandbox-test}"
HERE="$OUT/bin"
mkdir -p "$HERE" "$OUT/results"
CC="${BENCH_CC:-$(ls -d "$REPO"/.native-build/toolchains/llvm-mingw-*/bin/x86_64-w64-mingw32-clang 2>/dev/null | head -1)}"
[ -x "$CC" ] || { echo "no x86_64-w64-mingw32-clang (set BENCH_CC)" >&2; exit 1; }
build() {  # build <out> <compiler> <source> [flags...]
  local out="$1" cc="$2" src="$3"; shift 3
  [ -f "$out" ] && [ "$out" -nt "$src" ] || "$cc" -O2 -o "$out" "$src" "$@"
}
build "$HERE/sbtest.exe" "$CC" "$SRC/sbtest.c" -lws2_32 -lwinhttp -lshell32
build "$HERE/iotest.exe" "$CC" "$SRC/iotest.c" -lwinmm -lxinput -ldinput8 -ldxguid -lole32
build "$HERE/d3d11_clear.exe" "$CC" "$REPO/tests/d3d11_clear.c" -ld3d11 -ldxgi
build "$HERE/probe" clang "$SRC/probe.c"
STEAM_ROOT="$HOME/Library/Application Support/Steam"
CEF_PORT=18080   # stands in for Steam's CEF port (the tool passes STEAM_CEF_PORT to the profile)
OPEN_PORT=18081
SOCK=/private/tmp/neutron-sbtest.sock

mkdir -p "$HOME/Documents/neutron-sandbox-canary"
echo "neutron sandbox canary, safe to delete" > "$HOME/Documents/neutron-sandbox-canary/canary.txt"
rm -f /private/tmp/neutron-sbtest-exec-sh /private/tmp/neutron-sbtest-exec-start "$SOCK"

python3 -I -m http.server "$CEF_PORT" --bind ::1 >/dev/null 2>&1 & L1=$!
python3 -I -m http.server "$CEF_PORT" --bind 127.0.0.1 >/dev/null 2>&1 & L2=$!
python3 -I -m http.server "$OPEN_PORT" --bind 127.0.0.1 >/dev/null 2>&1 & L3=$!
python3 -I -c "import socket,time; s=socket.socket(socket.AF_UNIX); s.bind('$SOCK'); s.listen(8); time.sleep(600)" & L4=$!
trap 'kill $L1 $L2 $L3 $L4 2>/dev/null; rm -f "$SOCK"; rm -rf "$HOME/Documents/neutron-sandbox-canary"' EXIT
sleep 1

run() { # <data dir name> <sandbox 0|1> <exe> [args]
  local data="$OUT/results/$1" sb="$2"; shift 2
  mkdir -p "$data"
  env SteamAppId=sbtest STEAM_COMPAT_DATA_PATH="$data" NEUTRON_FILES="$FILES" \
    NEUTRON_SANDBOX="$sb" NEUTRON_SANDBOX_EXTRA_EXEC="$HERE/probe" STEAM_CEF_PORT="$CEF_PORT" \
    "$TOOL" runinprefix "$@"
}

# name, expected without sandbox, expected with sandbox; * = not compared (the
# result depends on the environment, e.g. an outer sandbox like Claude Code's)
EXPECT="
read_documents ALLOWED DENIED
list_documents ALLOWED DENIED
list_home ALLOWED DENIED
write_desktop ALLOWED DENIED
write_home ALLOWED DENIED
read_system ALLOWED ALLOWED
read_steam_registry ALLOWED ALLOWED
write_prefix ALLOWED ALLOWED
write_my_documents ALLOWED ALLOWED
write_game_dir ALLOWED ALLOWED
exec_sh ran blocked
exec_shellexecute ran blocked
tcp_cef_port ALLOWED DENIED
tcp_cef_port_v6 ALLOWED DENIED
tcp_localhost_other ALLOWED ALLOWED
tcp_internet ALLOWED ALLOWED
https_winhttp ALLOWED ALLOWED
lsopen ALLOWED DENIED
appleevent_send ALLOWED DENIED
job_creation ALLOWED DENIED
exec_bin_sh ALLOWED DENIED
exec_open ALLOWED DENIED
exec_osascript ALLOWED DENIED
read_ssh_dir ALLOWED DENIED
read_keychains_dir ALLOWED DENIED
write_launchagents * DENIED
read_steam_config ALLOWED DENIED
read_steam_local ALLOWED DENIED
read_steam_reg_native ALLOWED ALLOWED
read_steamclient ALLOWED ALLOWED
write_steam_cloud_app * *
write_steam_cloud_other * DENIED
write_steam_pipe ALLOWED DENIED
mach_steam_ipctool ALLOWED ALLOWED
unix_other_socket ALLOWED DENIED
"

declare -a RES0 RES1
for sb in 0 1; do
  rm -f /private/tmp/neutron-sbtest-exec-sh /private/tmp/neutron-sbtest-exec-start
  out="$(run "sb$sb" "$sb" "$HERE/sbtest.exe" "$HOME" "$STEAM_ROOT" "$CEF_PORT" "$OPEN_PORT" "$HERE/probe" 2>/dev/null | tr -d '\r')"
  # exec checks: did the command really run (marker file)?
  for m in sh start; do
    if [ -f "/private/tmp/neutron-sbtest-exec-$m" ]; then r=ran; else r=blocked; fi
    out="$(printf '%s\nexec_%s %s\n' "$out" "$( [ $m = sh ] && echo sh || echo shellexecute )" "$r")"
  done
  printf '%s\n' "$out" > "$OUT/results/sbtest-result-$sb.txt"
  # Are the wineservers in the sandbox? A server lingers a few seconds after its last
  # client. Our profile is the one that denies the canary; other servers (other
  # prefixes, other sessions) show ALLOWED.
  for pid in $(pgrep -f "/bin/wineserver" || true); do
    printf 'NEUTRON_SANDBOX=%s wineserver %s\n' "$sb" \
      "$("$HERE/probe" pid "$HOME/Documents/neutron-sandbox-canary/canary.txt" "$pid")"
  done
done

printf '\n%-24s %-9s %-9s %-9s %-9s %s\n' check "exp off" "got off" "exp on" "got on" ""
fail=0
while read -r name e0 e1; do
  [ -n "$name" ] || continue
  g0="$(awk -v n="$name" '$1 == n { print $2 }' "$OUT/results/sbtest-result-0.txt" | tail -1)"
  g1="$(awk -v n="$name" '$1 == n { print $2 }' "$OUT/results/sbtest-result-1.txt" | tail -1)"
  ok=ok
  { [ "$e0" = "*" ] || [ "$g0" = "$e0" ]; } && { [ "$e1" = "*" ] || [ "$g1" = "$e1" ]; } || { ok=MISMATCH; fail=1; }
  printf '%-24s %-9s %-9s %-9s %-9s %s\n' "$name" "$e0" "${g0:--}" "$e1" "${g1:--}" "$ok"
done <<< "$EXPECT"

# Steam Cloud and stats write rules against a stand-in Steam root (the real one is
# outside what this test may write), straight through sandbox-exec.
FAKE="$OUT/results/fakesteam"
mkdir -p "$FAKE/userdata/1/sbtest/remote" "$FAKE/userdata/1/7/remote" "$FAKE/appcache/stats" "$FAKE/config"
echo
echo "write rules, stand-in Steam root:"
PROBE_REAL_WRITES=1 /usr/bin/sandbox-exec -f "$(dirname "$TOOL")/neutron.sb" -D HOME="$HOME" -D TOOL_DIR="$(dirname "$TOOL")" \
  -D FILES="$FILES" -D COMPAT="$OUT/results/sb1" -D GAME="" -D STEAM_ROOT="$FAKE" -D APPID=sbtest \
  -D LOGS="$HOME/Library/Logs/neutron" -D SERVER_DIR=/private/tmp/.wine-0/none -D CEF_PORT="$CEF_PORT" -D LOCALHOST=allow \
  -D EXTRA_EXEC="$HERE/probe" "$HERE/probe" check "$HOME" "$FAKE" | grep -E "steam_cloud|steam_config|steam_local"
echo "(expect write_steam_cloud_app ALLOWED, write_steam_cloud_other DENIED, config/local DENIED)"

echo
echo "d3d11_clear.exe in the sandbox:"
run sb1 1 "$HERE/d3d11_clear.exe" 2>/dev/null | grep -E "PASS|FAIL|pixel" || fail=1
echo
echo "iotest.exe in the sandbox (CoreAudio silence, DirectInput/IOHID, XInput):"
run sb1 1 "$HERE/iotest.exe" 2>/dev/null | tr -d '\r' || fail=1
exit $fail
