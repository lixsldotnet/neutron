#!/usr/bin/env bash
#
# Runs after Mac Steam started with Steam Play turned on (steam_dev.cfg with platform
# "linux", CEF debug port from STEAM_CEF_PORT): waits for the Steam UI, sets the
# platform back to macOS so Mac games keep their Mac depots, remaps the games
# mapped to neutron (Steam marks them "invalid platform" after the switch), maps
# new games without a Mac version (steam-sync.js) and adds the Compatibility tab
# with the neutron settings to the game properties (steam-panel.js).
#
# Started by the Steam.app start script that install.sh puts in place.
# Usage: steam-hook.sh [tool name]    (default neutron_proton)
#
set -euo pipefail

DEV_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOL="${1:-neutron_proton}"
CONFIG_VDF="$HOME/Library/Application Support/Steam/config/config.vdf"

say() { printf '%s %s\n' "$(date '+%F %T')" "$*"; }
js()  { node "$DEV_DIR/steamjs.mjs" "$1"; }
# Larger snippets live in files: macOS bash 3.2 garbles long JS inside "$(...)".
js_file() { js "$(sed -e "s/__TOOL__/$TOOL/g" -e "s/__MAPPED__/${MAPPED:-}/g" "$DEV_DIR/$1")"; }

# App ids mapped to the tool (or its _remap name, see steam-sync.js) in config.vdf's
# CompatToolMapping, comma separated.
mapped_apps() {
  awk -v tool="\"$TOOL\"" -v remap="\"${TOOL}_remap\"" '
    /"CompatToolMapping"/ { inmap = 1; depth = 0; next }
    !inmap { next }
    /^[ \t]*\{/ { depth++; next }
    /^[ \t]*\}/ { if (--depth <= 0) exit; next }
    depth == 1 && match($0, /"[0-9]+"/) { id = substr($0, RSTART + 1, RLENGTH - 2) }
    depth == 2 && $1 == "\"name\"" && ($2 == tool || $2 == remap) && id != "0" { print id }
  ' "$CONFIG_VDF" 2>/dev/null | paste -sd, -
}

ui_up() { node "$DEV_DIR/steamjs.mjs" 2>/dev/null | grep -q '^SharedJSContext |'; }
for _ in $(seq 1 150); do ui_up && break; sleep 2; done
ui_up || { say "Steam UI did not come up"; exit 1; }

say "Compat layer: $(js 'new Promise(r=>SteamClient.Settings.RegisterForSettingsChanges(s=>r(s.bCompatEnabled)))')"
js 'new Promise(r=>{SteamClient.Console.ExecCommand("@sSteamCmdForcePlatformType macos"); setTimeout(()=>r(1),1000)})' >/dev/null
say "Platform back to macos, tools: $(js 'SteamClient.Settings.GetGlobalCompatTools()')"
MAPPED="$(mapped_apps)"
say "Games: $(js_file steam-sync.js)"
say "Settings tab: $(js "$(cat "$DEV_DIR/steam-panel.js")")"

# Steam reloads its UI context now and then (new CLIENT_SESSION), which drops the
# tab and the new-game watch: put them back while this Steam runs. A hook from a
# newer Steam start takes over.
PIDFILE="$HOME/Library/Logs/neutron/steam-hook.pid"
echo $$ > "$PIDFILE"
while pgrep -x steam_osx >/dev/null && [ "$(cat "$PIDFILE" 2>/dev/null)" = $$ ]; do
  sleep 10
  [ "$(js 'window.__neutronPanel === true' 2>/dev/null)" = false ] || continue
  MAPPED=""
  say "Steam UI reloaded, games: $(js_file steam-sync.js)"
  say "Steam UI reloaded, settings tab: $(js "$(cat "$DEV_DIR/steam-panel.js")")"
done
