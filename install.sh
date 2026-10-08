#!/usr/bin/env bash
#
# Sets up neutron for the Mac Steam client of this user:
#   - the compat tool in ~/Library/Application Support/neutron/compatibilitytools.d/neutron_proton
#     (from a build, or as links into the dev tree with --dev); neutron.env is kept
#   - the helper scripts in ~/Library/Application Support/neutron/bin
#   - a start script in /Applications/Steam.app, so every Steam start (Dock, login,
#     Steam's own restart) comes up with Steam Play for neutron: Steam.app's
#     CFBundleExecutable points to Contents/MacOS/steam_neutron, which turns Steam
#     Play on, starts dev/steam-hook.sh (platform back to macOS, mapping refresh,
#     Compatibility tab) and runs Valve's unchanged steam_osx with the CEF debug port.
#     Valve's steam_osx is bound to Info.plist, so it and the bundle are signed ad hoc
#     afterwards (like NotProton does); steam_osx, Info.plist and _CodeSignature are
#     backed up first and --uninstall puts Valve's originals back.
#   - then starts Steam and maps all games without a Mac version
#
# A Steam client update can replace Steam.app: run ./install.sh again.
#
# Usage: ./install.sh [--dist <dist/neutron>] [--dev] [--no-start]
#        ./install.sh --uninstall      (puts Steam.app back, removes neutron, keeps game prefixes)
# Default build: .native-build/dist/neutron (./build.sh)
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HOME_DIR="$HOME/Library/Application Support/neutron"
TOOL_DIR="$HOME_DIR/compatibilitytools.d/neutron_proton"
BIN="$HOME_DIR/bin"
BACKUP="$HOME_DIR/steam-app-backup"
LOGS="$HOME/Library/Logs/neutron"
STEAM_APP="/Applications/Steam.app"
STEAM_BIN="$HOME/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS"
# from earlier versions of this script
OLD_APP="$HOME/Applications/Steam (Neutron).app"
OLD_AGENT="$HOME/Library/LaunchAgents/net.lixsl.neutron.steam.plist"

DIST="$ROOT/.native-build/dist/neutron"
DEV=0 START=1 UNINSTALL=0

say() { printf '\033[36m> %s\033[0m\n' "$*"; }
die() { printf '\033[31mx %s\033[0m\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
  case "$1" in
    --dist) DIST="$2"; shift 2 ;;
    --dev) DEV=1; shift ;;
    --no-start) START=0; shift ;;
    --uninstall) UNINSTALL=1; shift ;;
    *) die "unknown option $1" ;;
  esac
done

quit_steam() {
  pgrep -f "Steam.AppBundle/Steam/Contents/MacOS/steam_osx" >/dev/null || return 0
  say "Quitting Steam"
  osascript -e 'quit app "Steam"' >/dev/null 2>&1 || true
  for _ in $(seq 1 30); do
    pgrep -f "Steam.AppBundle/Steam/Contents/MacOS/steam_osx" >/dev/null || return 0
    sleep 1
  done
  die "Steam did not quit, quit it and run this again"
}

lsregister() {
  /System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$STEAM_APP"
}

remove_old() {
  rm -rf "$OLD_APP"
  if [ -f "$OLD_AGENT" ]; then
    launchctl bootout "gui/$(id -u)" "$OLD_AGENT" 2>/dev/null || true
    rm -f "$OLD_AGENT"
  fi
}

unpatch_steam() {
  [ -f "$BACKUP/Info.plist" ] || return 0
  say "Putting Steam.app back"
  cp "$BACKUP/Info.plist" "$STEAM_APP/Contents/Info.plist"
  rm -rf "$STEAM_APP/Contents/_CodeSignature"
  cp -R "$BACKUP/_CodeSignature" "$STEAM_APP/Contents/_CodeSignature"
  rm -f "$STEAM_APP/Contents/MacOS/steam_neutron" "$STEAM_APP/Contents/MacOS/steam_osx"
  cp "$BACKUP/steam_osx" "$STEAM_APP/Contents/MacOS/steam_osx"
  rm -rf "$BACKUP"
  lsregister
}

patch_steam() {
  local exe plist node_dir
  plist="$STEAM_APP/Contents/Info.plist"
  [ -x "$STEAM_APP/Contents/MacOS/steam_osx" ] || die "no Steam in /Applications (install Mac Steam first)"
  exe="$(plutil -extract CFBundleExecutable raw "$plist")"
  # Back up Valve's bundle files once; a Steam update brings back steam_osx, back up again then.
  if [ "$exe" = steam_osx ]; then
    rm -rf "$BACKUP"
    mkdir -p "$BACKUP"
    cp "$plist" "$BACKUP/Info.plist"
    cp -R "$STEAM_APP/Contents/_CodeSignature" "$BACKUP/_CodeSignature"
    cp "$STEAM_APP/Contents/MacOS/steam_osx" "$BACKUP/steam_osx"
  fi
  [ -f "$BACKUP/Info.plist" ] || die "Steam.app is changed but no backup exists; reinstall Mac Steam"

  say "Installing the start script into Steam.app"
  node_dir="$(dirname "$(command -v node)")"
  cat > "$STEAM_APP/Contents/MacOS/steam_neutron" <<EOF
#!/bin/bash
# neutron: starts Valve's steam_osx with Steam Play for neutron turned on. Installed by
# neutron's install.sh; './install.sh --uninstall' puts Steam.app back.
N="\$HOME/Library/Application Support/neutron"
export STEAM_EXTRA_COMPAT_TOOLS_PATHS="\$N/compatibilitytools.d"
printf '@sSteamCmdForcePlatformType linux\\n' > "\$HOME/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/steam_dev.cfg" 2>/dev/null
mkdir -p "\$HOME/Library/Logs/neutron"
PATH="$node_dir:/usr/bin:/bin:/usr/sbin:/sbin" "\$N/bin/steam-hook.sh" >> "\$HOME/Library/Logs/neutron/steam-hook.log" 2>&1 &
exec "\$(dirname "\$0")/steam_osx" -cef-enable-debugging -devtools-port 8080 "\$@"
EOF
  chmod +x "$STEAM_APP/Contents/MacOS/steam_neutron"
  plutil -replace CFBundleExecutable -string steam_neutron "$plist"
  codesign -f -s - "$STEAM_APP/Contents/MacOS/steam_osx" 2>&1 | grep -v "replacing existing signature" || true
  codesign -f -s - "$STEAM_APP" 2>&1 | grep -v "replacing existing signature" || true
  codesign --verify "$STEAM_APP" || die "Steam.app signature check failed; ./install.sh --uninstall puts it back"
  lsregister
}

if [ "$UNINSTALL" = 1 ]; then
  quit_steam
  unpatch_steam
  remove_old
  rm -rf "$BIN" "$TOOL_DIR"
  rm -f "$STEAM_BIN/steam_dev.cfg"
  say "Removed. Game prefixes stay in steamapps/compatdata/<appid>/pfx."
  exit 0
fi

command -v node >/dev/null || die "node is needed (brew install node)"
[ -d "$STEAM_BIN" ] || die "start Mac Steam once and log in first"

say "Installing the compat tool"
mkdir -p "$TOOL_DIR" "$LOGS"
for f in neutron files compatibilitytool.vdf toolmanifest.vdf; do rm -rf "${TOOL_DIR:?}/$f"; done
if [ "$DEV" = 1 ]; then
  [ -d "$ROOT/.native-build/wine-install/lib/wine/aarch64-windows" ] || die "no dev runtime in .native-build/wine-install"
  ln -s "$ROOT/tool/neutron" "$TOOL_DIR/neutron"
  ln -s "$ROOT/.native-build/wine-install" "$TOOL_DIR/files"
  cp "$ROOT/tool/compatibilitytool.vdf" "$ROOT/tool/toolmanifest.vdf" "$TOOL_DIR/"
else
  [ -x "$DIST/neutron" ] && [ -d "$DIST/files" ] || die "no build at $DIST (run ./build.sh, or --dev)"
  cp -R "$DIST/files" "$TOOL_DIR/files"
  cp "$DIST/neutron" "$DIST/peicon.py" "$DIST/compatibilitytool.vdf" "$DIST/toolmanifest.vdf" "$TOOL_DIR/"
fi
[ -f "$TOOL_DIR/neutron.env" ] || printf '# KEY=value lines for all games, e.g. NEUTRON_HUD=1\n' > "$TOOL_DIR/neutron.env"

say "Installing the helper scripts"
mkdir -p "$BIN"
cp "$ROOT/dev/steam.sh" "$ROOT/dev/steam-hook.sh" "$ROOT/dev/steamjs.mjs" "$ROOT"/dev/steam-*.js "$BIN/"
chmod +x "$BIN/steam.sh" "$BIN/steam-hook.sh"

quit_steam
remove_old
patch_steam

if [ "$START" = 1 ]; then
  say "Starting Steam (maps all games without a Mac version)"
  "$BIN/steam.sh" --map-all
fi
say "Done. Start Steam as usual; after a Steam update run ./install.sh again."
