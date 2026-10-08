#!/usr/bin/env bash
#
# Restarts the Mac Steam client set up by install.sh (start script in Steam.app,
# which turns Steam Play on and runs steam-hook.sh) and waits for the hook: platform
# back to macOS, games remapped and new games mapped, Compatibility tab.
#
# Usage: dev/steam.sh [--tool <name>] [--map <appid>]... [--launch <appid>=<options>]...
#                     [--global <name>|none]
#   --map     maps a game to the tool (games without a Mac version are mapped
#             automatically, see steam-sync.js)
#   --tool    compat tool name for --map (default neutron_proton)
#   --global  default compat tool for all Windows games (none removes it). This alone
#             does not unlock installing a Windows-only game, the per-game mapping does.
#   --launch  per-game launch options. NEUTRON_<NAME>=<value> words there are neutron
#             settings for that game (Mac Steam does not support "VAR=x %command%");
#             neutron.env next to the tool holds settings for all games.
#
set -euo pipefail

DEV_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MAP_APPS=()
LAUNCH_OPTS=()
TOOL="neutron_proton"
GLOBAL_TOOL=""

say() { printf '\033[36m> %s\033[0m\n' "$*"; }
die() { printf '\033[31mx %s\033[0m\n' "$*" >&2; exit 1; }
js()  { node "$DEV_DIR/steamjs.mjs" "$1"; }   # CEF port: see steamjs.mjs

while [ $# -gt 0 ]; do
  case "$1" in
    --map)  MAP_APPS+=("$2"); shift 2 ;;
    --tool) TOOL="$2"; shift 2 ;;
    --launch) LAUNCH_OPTS+=("$2"); shift 2 ;;
    --global) GLOBAL_TOOL="$2"; shift 2 ;;
    *)      die "unknown option $1" ;;
  esac
done

[ "$(plutil -extract CFBundleExecutable raw /Applications/Steam.app/Contents/Info.plist 2>/dev/null)" = steam_neutron ] ||
  die "Steam.app has no neutron start script, run ./install.sh"
command -v node >/dev/null || die "node is needed for the CEF calls (brew install node)"

if pgrep -x steam_osx >/dev/null; then
  say "Quitting Steam"
  osascript -e 'quit app "Steam"' >/dev/null 2>&1 || true
  for _ in $(seq 1 20); do pgrep -x steam_osx >/dev/null || break; sleep 1; done
  pgrep -x steam_osx >/dev/null && die "Steam did not quit"
fi

HOOK_LOG="$HOME/Library/Logs/neutron/steam-hook.log"
mkdir -p "$(dirname "$HOOK_LOG")"; touch "$HOOK_LOG"
start_at=$(wc -c < "$HOOK_LOG")
say "Starting Steam"
open -a Steam
for _ in $(seq 1 180); do
  tail -c +"$((start_at + 1))" "$HOOK_LOG" | grep -q "Settings tab:" && break
  sleep 2
done
tail -c +"$((start_at + 1))" "$HOOK_LOG" | sed 's/^[0-9-]* [0-9:]* /  /'

for app in ${MAP_APPS[@]+"${MAP_APPS[@]}"}; do
  say "Mapping $app to $TOOL"
  js "SteamClient.Apps.SpecifyCompatTool($app, \"$TOOL\")" >/dev/null
done
for entry in ${LAUNCH_OPTS[@]+"${LAUNCH_OPTS[@]}"}; do
  say "Launch options for ${entry%%=*}: ${entry#*=}"
  js "SteamClient.Apps.SetAppLaunchOptions(${entry%%=*}, \"${entry#*=}\")" >/dev/null
done
if [ "$GLOBAL_TOOL" = none ]; then
  say "Removing the global compat tool"
  js 'SteamClient.Settings.SpecifyGlobalCompatTool("")' >/dev/null
elif [ -n "$GLOBAL_TOOL" ]; then
  say "Global compat tool: $GLOBAL_TOOL"
  js "SteamClient.Settings.SpecifyGlobalCompatTool(\"$GLOBAL_TOOL\")" >/dev/null
fi
say "Ready. Logs: ~/Library/Logs/neutron/"
