#!/usr/bin/env bash
#
# Starts the Mac Steam client with Steam Play turned on for neutron (dev setup).
#
#   1. installs a neutron build into ~/Library/Application Support/neutron
#   2. writes steam_dev.cfg into Steam's binaries folder (platform "linux" at
#      startup, which turns the compat layer on)
#   3. starts Steam with the CEF debug port and STEAM_EXTRA_COMPAT_TOOLS_PATHS
#   4. after login, sets the platform back to "macos" so Mac games keep Mac depots
#   5. optional: maps games to neutron
#
# Usage: dev/steam.sh [--dist <dist/neutron>] [--tool <name>] [--map <appid>]... [--map-all]
#                     [--launch <appid>=<options>]... [--global <name>|none]
#   --map-all maps every game in the library that has no Mac version
#   --tool    compat tool name for --map and --map-all (default neutron_proton)
#   --global  default compat tool for all Windows games (none removes it). This alone
#             does not unlock installing a Windows-only game, the per-game mapping does.
#   --launch  per-game launch options (Mac Steam ignores "VAR=x %command%", use neutron.env)
# Steam must have run once (so Steam.AppBundle exists) and be logged in.
#
set -euo pipefail

DEV_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STEAM_ROOT="$HOME/Library/Application Support/Steam"
STEAM_BIN="$STEAM_ROOT/Steam.AppBundle/Steam/Contents/MacOS"
TOOLS_DIR="$HOME/Library/Application Support/neutron/compatibilitytools.d"
CEF_PORT=8080
DIST=""
MAP_APPS=()
MAP_ALL=0
LAUNCH_OPTS=()
TOOL="neutron_proton"
GLOBAL_TOOL=""

say() { printf '\033[36m> %s\033[0m\n' "$*"; }
die() { printf '\033[31mx %s\033[0m\n' "$*" >&2; exit 1; }
js()  { STEAM_CEF_PORT=$CEF_PORT node "$DEV_DIR/steamjs.mjs" "$1"; }

while [ $# -gt 0 ]; do
  case "$1" in
    --dist) DIST="$2"; shift 2 ;;
    --map)  MAP_APPS+=("$2"); shift 2 ;;
    --map-all) MAP_ALL=1; shift ;;
    --tool) TOOL="$2"; shift 2 ;;
    --launch) LAUNCH_OPTS+=("$2"); shift 2 ;;
    --global) GLOBAL_TOOL="$2"; shift 2 ;;
    *)      die "unknown option $1" ;;
  esac
done

[ -d "$STEAM_BIN" ] || die "Steam has not run yet: start it once and log in, then run this again."
command -v node >/dev/null || die "node is needed for the CEF calls (brew install node)"

if [ -n "$DIST" ]; then
  [ -x "$DIST/neutron" ] || die "no neutron build at $DIST"
  # Folder name = tool name from the dist's compatibilitytool.vdf.
  name="$(awk -F'"' '/"compat_tools"/ {f=1; next} f && $2 != "" {print $2; exit}' "$DIST/compatibilitytool.vdf")"
  [ -n "$name" ] || die "no tool name in $DIST/compatibilitytool.vdf"
  say "Installing $DIST as $name"
  mkdir -p "$TOOLS_DIR"
  rm -rf "${TOOLS_DIR:?}/$name"
  cp -R "$DIST" "$TOOLS_DIR/$name"
fi
[ -x "$TOOLS_DIR/$TOOL/neutron" ] || die "$TOOL is not installed, pass --dist <dist>"

say "Writing steam_dev.cfg"
printf '@sSteamCmdForcePlatformType linux\n' > "$STEAM_BIN/steam_dev.cfg"

if pgrep -x steam_osx >/dev/null; then
  say "Quitting Steam"
  osascript -e 'quit app "Steam"' >/dev/null 2>&1 || true
  for _ in $(seq 1 20); do pgrep -x steam_osx >/dev/null || break; sleep 1; done
  pgrep -x steam_osx >/dev/null && die "Steam did not quit"
fi

say "Starting Steam"
open --env "STEAM_EXTRA_COMPAT_TOOLS_PATHS=$TOOLS_DIR" -a Steam \
  --args -cef-enable-debugging -devtools-port "$CEF_PORT"

say "Waiting for the Steam UI"
for _ in $(seq 1 90); do
  curl -sf "http://127.0.0.1:$CEF_PORT/json" 2>/dev/null | grep -q SharedJSContext && break
  sleep 2
done
curl -sf "http://127.0.0.1:$CEF_PORT/json" 2>/dev/null | grep -q SharedJSContext || die "Steam UI did not come up"

say "Compat layer: $(js 'new Promise(r=>SteamClient.Settings.RegisterForSettingsChanges(s=>r(s.bCompatEnabled)))')"
say "Platform back to macos"
js 'new Promise(r=>{SteamClient.Console.ExecCommand("@sSteamCmdForcePlatformType macos"); setTimeout(()=>r(1),1000)})' >/dev/null
say "Tools: $(js 'SteamClient.Settings.GetGlobalCompatTools()')"

# After the platform switch Steam keeps "invalid platform" for games mapped to the
# tool until the mapping is set again.
say "Refreshing games mapped to $TOOL: $(js "(async () => {
  const games = () => (window.collectionStore?.allGamesCollection?.allApps || []).filter(a => a.app_type == 1);
  for (let i = 0; i < 120 && !games().length; i++) await new Promise(r => setTimeout(r, 500));
  const tool = id => new Promise(resolve => {
    let done = false;
    const h = SteamClient.Apps.RegisterForAppDetails(id, d => { if (done) return; done = true; h.unregister(); resolve(d.strCompatToolName); });
    setTimeout(() => { if (!done) { done = true; h.unregister(); resolve(''); } }, 3000);
  });
  let n = 0;
  for (const app of games()) {
    if (await tool(app.appid) !== '$TOOL') continue;
    SteamClient.Apps.SpecifyCompatTool(app.appid, '');
    await new Promise(r => setTimeout(r, 500));
    SteamClient.Apps.SpecifyCompatTool(app.appid, '$TOOL');
    await new Promise(r => setTimeout(r, 100));
    n++;
  }
  return n;
})()")"

for app in ${MAP_APPS[@]+"${MAP_APPS[@]}"}; do
  say "Mapping $app to $TOOL"
  js "SteamClient.Apps.SpecifyCompatTool($app, \"$TOOL\")" >/dev/null
done
if [ "$MAP_ALL" = 1 ]; then
  say "Mapping all games without a Mac version to $TOOL"
  js "(async () => {
    const details = id => new Promise(resolve => {
      let done = false;
      const h = SteamClient.Apps.RegisterForAppDetails(id, d => { if (done) return; done = true; h.unregister(); resolve(d); });
      setTimeout(() => { if (!done) { done = true; h.unregister(); resolve(null); } }, 3000);
    });
    const games = () => (window.collectionStore?.allGamesCollection?.allApps || []).filter(a => a.app_type == 1);
    for (let i = 0; i < 120 && !games().length; i++) await new Promise(r => setTimeout(r, 500));
    let mapped = 0, mac = 0, unknown = 0;
    for (const app of games()) {
      const d = await details(app.appid);
      if (!d || !d.vecPlatforms || !d.vecPlatforms.length) { unknown++; continue; }
      if (d.strCompatToolName === '$TOOL') continue;
      // A mapped compat tool makes Steam list osx too: games mapped to another
      // (older) tool name are Windows games, remap them.
      if (d.vecPlatforms.includes('osx') && !d.strCompatToolName) { mac++; continue; }
      SteamClient.Apps.SpecifyCompatTool(app.appid, '$TOOL'); mapped++;
    }
    return 'mapped ' + mapped + ', Mac native ' + mac + ', unknown ' + unknown;
  })()"
fi
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
