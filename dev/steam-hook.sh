#!/usr/bin/env bash
#
# Runs after Mac Steam started with Steam Play turned on (steam_dev.cfg with platform
# "linux", CEF debug port): waits for the Steam UI, sets the platform back to macOS
# so Mac games keep their Mac depots, refreshes the games mapped to neutron (Steam
# marks them "invalid platform" after the switch) and adds the Compatibility tab with
# the neutron settings to the game properties.
#
# Started by the Steam.app start script that install.sh puts in place, and by
# dev/steam.sh. Usage: steam-hook.sh [tool name]    (default neutron_proton)
#
set -euo pipefail

DEV_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOL="${1:-neutron_proton}"
CEF_PORT=8080

say() { printf '%s %s\n' "$(date '+%F %T')" "$*"; }
js()  { STEAM_CEF_PORT=$CEF_PORT node "$DEV_DIR/steamjs.mjs" "$1"; }
# Larger snippets live in files: macOS bash 3.2 garbles long JS inside "$(...)".
js_file() { js "$(sed "s/__TOOL__/$TOOL/g" "$DEV_DIR/$1")"; }

ui_up() { curl -sf "http://127.0.0.1:$CEF_PORT/json" 2>/dev/null | grep -q SharedJSContext; }
for _ in $(seq 1 150); do ui_up && break; sleep 2; done
ui_up || { say "Steam UI did not come up"; exit 1; }

say "Compat layer: $(js 'new Promise(r=>SteamClient.Settings.RegisterForSettingsChanges(s=>r(s.bCompatEnabled)))')"
js 'new Promise(r=>{SteamClient.Console.ExecCommand("@sSteamCmdForcePlatformType macos"); setTimeout(()=>r(1),1000)})' >/dev/null
say "Platform back to macos, tools: $(js 'SteamClient.Settings.GetGlobalCompatTools()')"
say "Refreshed games mapped to $TOOL: $(js_file steam-refresh.js)"
say "Settings tab: $(js "$(cat "$DEV_DIR/steam-panel.js")")"
