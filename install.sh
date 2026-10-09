#!/usr/bin/env bash
#
# Sets up neutron for the Mac Steam client of this user:
#   - the compat tool in ~/Library/Application Support/neutron/compatibilitytools.d/neutron_proton
#     (from a build, or as links into the dev tree with --dev); neutron.env is kept
#   - the helper scripts in ~/Library/Application Support/neutron/bin
#   - a start script in /Applications/Steam.app, so every Steam start (Dock, login,
#     Steam's own restart) comes up with Steam Play for neutron: Steam.app's
#     CFBundleExecutable points to Contents/MacOS/steam_neutron, which turns Steam
#     Play on, starts dev/steam-hook.sh (platform back to macOS, games remapped and
#     new games mapped, Compatibility tab) and runs Valve's unchanged steam_osx with
#     the CEF debug port (localhost).
#     Valve's steam_osx is bound to Info.plist, so it and the bundle are signed ad hoc
#     afterwards (like NotProton does); steam_osx, Info.plist and _CodeSignature are
#     backed up first and --uninstall puts Valve's originals back.
#   - then starts Steam; the hook maps all games without a Mac version
#
# A Steam client update can replace Steam.app: run ./install.sh again.
#
# Usage: ./install.sh [--dist <dist/neutron>] [--dev] [--no-start] [--cef-port <port>]
#        ./install.sh --uninstall      (puts Steam.app back, removes neutron, keeps game prefixes)
#   --dist      tool folder (default: dist/neutron in a release download,
#               else .native-build/dist/neutron from build.sh)
#   --dev       links tool/neutron from this repo instead of copying it (see below)
#   --cef-port  CEF debug port of Steam (default 8080), the helper scripts read it
#               from the start script
#
# --uninstall leaves the games mapped to neutron_proton in Steam's config on
# purpose: setting an installed Windows-only game to no tool makes Steam delete
# its files.
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

# A release download has the tool in dist/neutron, a source build in .native-build/dist/neutron.
DIST="$ROOT/.native-build/dist/neutron"
[ -d "$ROOT/dist/neutron" ] && DIST="$ROOT/dist/neutron"
CEF_PORT=8080
DEV=0 START=1 UNINSTALL=0

say() { printf '\033[36m> %s\033[0m\n' "$*"; }
die() { printf '\033[31mx %s\033[0m\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
  case "$1" in
    --dist) DIST="$2"; shift 2 ;;
    --dev) DEV=1; shift ;;
    --no-start) START=0; shift ;;
    --cef-port) CEF_PORT="$2"; shift 2 ;;
    --uninstall) UNINSTALL=1; shift ;;
    *) die "unknown option $1" ;;
  esac
done
case "$CEF_PORT" in ''|*[!0-9]*) die "--cef-port needs a port number" ;; esac

# Both Valve binaries are called steam_osx: the bootstrapper in Steam.app and the
# client in Steam.AppBundle. Neither may run while Steam.app is changed.
quit_steam() {
  pgrep -x steam_osx >/dev/null || return 0
  say "Quitting Steam"
  osascript -e 'quit app "Steam"' >/dev/null 2>&1 || true
  for _ in $(seq 1 30); do
    pgrep -x steam_osx >/dev/null || return 0
    sleep 1
  done
  die "Steam did not quit, quit it and run this again"
}

lsregister() {
  /System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$STEAM_APP"
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
# Start script from neutron's install.sh: starts Valve's steam_osx with Steam Play for
# neutron turned on. './install.sh --uninstall' puts Steam.app back.
N="\$HOME/Library/Application Support/neutron"
export STEAM_EXTRA_COMPAT_TOOLS_PATHS="\$N/compatibilitytools.d"
# CEF debug port, read by the helper scripts (steamjs.mjs)
export STEAM_CEF_PORT=$CEF_PORT
printf '@sSteamCmdForcePlatformType linux\\n' > "\$HOME/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/steam_dev.cfg" 2>/dev/null
mkdir -p "\$HOME/Library/Logs/neutron"
PATH="$node_dir:/usr/bin:/bin:/usr/sbin:/sbin" "\$N/bin/steam-hook.sh" >> "\$HOME/Library/Logs/neutron/steam-hook.log" 2>&1 &
exec "\$(dirname "\$0")/steam_osx" -cef-enable-debugging -devtools-port "\$STEAM_CEF_PORT" "\$@"
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
  rm -rf "$BIN" "$TOOL_DIR"
  rm -f "$STEAM_BIN/steam_dev.cfg"
  say "Removed. Game prefixes stay in steamapps/compatdata/<appid>/pfx."
  say "Games stay mapped to neutron_proton: setting an installed Windows-only game to no tool makes Steam delete its files."
  exit 0
fi

command -v node >/dev/null || die "node is needed (brew install node)"
[ -d "$STEAM_BIN" ] || die "start Mac Steam once and log in first"

say "Installing the compat tool"
mkdir -p "$TOOL_DIR" "$LOGS"
for f in neutron neutron.sb peicon.py files compatibilitytool.vdf toolmanifest.vdf; do rm -rf "${TOOL_DIR:?}/$f"; done
if [ "$DEV" = 1 ]; then
  # Dev install: links tool/neutron from this repo, so script changes apply at the next
  # game start, and links the runtime of the build (.native-build/dist/neutron/files),
  # or, without one, a hand-built Wine install in .native-build/wine-install.
  runtime="$ROOT/.native-build/dist/neutron/files"
  [ -d "$runtime" ] || runtime="$ROOT/.native-build/wine-install"
  [ -d "$runtime/lib/wine/aarch64-windows" ] || die "no runtime in $runtime (run ./build.sh)"
  ln -s "$ROOT/tool/neutron" "$TOOL_DIR/neutron"
  ln -s "$ROOT/tool/neutron.sb" "$TOOL_DIR/neutron.sb"
  ln -s "$runtime" "$TOOL_DIR/files"
  cp "$ROOT/tool/compatibilitytool.vdf" "$ROOT/tool/toolmanifest.vdf" "$TOOL_DIR/"
else
  [ -x "$DIST/neutron" ] && [ -d "$DIST/files" ] || die "no build at $DIST (run ./build.sh, or --dev)"
  cp -R "$DIST/files" "$TOOL_DIR/files"
  cp "$DIST/neutron" "$DIST/neutron.sb" "$DIST/peicon.py" "$DIST/compatibilitytool.vdf" "$DIST/toolmanifest.vdf" "$TOOL_DIR/"
  # Downloaded files carry the quarantine flag, then Gatekeeper refuses Wine's libraries.
  xattr -dr com.apple.quarantine "$TOOL_DIR" 2>/dev/null || true
fi
[ -f "$TOOL_DIR/neutron.env" ] || printf '# KEY=value lines for all games, e.g. NEUTRON_HUD=1\n' > "$TOOL_DIR/neutron.env"

say "Installing the helper scripts"
mkdir -p "$BIN"
rm -f "$BIN"/steam-*.js
cp "$ROOT/dev/steam.sh" "$ROOT/dev/steam-hook.sh" "$ROOT/dev/steamjs.mjs" "$ROOT/dev/steam-native.mjs" "$ROOT"/dev/steam-*.js "$BIN/"
chmod +x "$BIN/steam.sh" "$BIN/steam-hook.sh"

quit_steam
patch_steam

if [ "$START" = 1 ]; then
  say "Starting Steam (maps all games without a Mac version)"
  "$BIN/steam.sh"
fi
say "Done. Start Steam as usual; after a Steam update run ./install.sh again."
