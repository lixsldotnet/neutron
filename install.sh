#!/usr/bin/env bash
#
# Sets up neutron for the Mac Steam client of this user:
#   - the compat tool in ~/Library/Application Support/neutron/compatibilitytools.d/neutron_proton
#     (from a build, or as links into the dev tree with --dev); neutron.env is kept
#   - the start scripts in ~/Library/Application Support/neutron/bin
#   - "Steam (Neutron)" in ~/Applications: starts Steam with neutron (for the Dock)
#   - a login item that starts Steam with neutron at login
#   - then starts Steam with neutron and maps all games without a Mac version
#
# Steam only finds neutron when it is started this way (dev/steam.sh); a plain
# start of Steam.app runs Mac games fine but not the Windows ones.
#
# Usage: ./install.sh [--dist <dist/neutron>] [--dev] [--no-login] [--no-start]
#        ./install.sh --uninstall      (removes all of the above, keeps game prefixes)
# Default build: .native-build/dist/neutron (./build.sh)
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HOME_DIR="$HOME/Library/Application Support/neutron"
TOOL_DIR="$HOME_DIR/compatibilitytools.d/neutron_proton"
BIN="$HOME_DIR/bin"
APP="$HOME/Applications/Steam (Neutron).app"
AGENT="$HOME/Library/LaunchAgents/net.lixsl.neutron.steam.plist"
LOGS="$HOME/Library/Logs/neutron"
STEAM_BIN="$HOME/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS"

DIST="$ROOT/.native-build/dist/neutron"
DEV=0 LOGIN=1 START=1 UNINSTALL=0

say() { printf '\033[36m> %s\033[0m\n' "$*"; }
die() { printf '\033[31mx %s\033[0m\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
  case "$1" in
    --dist) DIST="$2"; shift 2 ;;
    --dev) DEV=1; shift ;;
    --no-login) LOGIN=0; shift ;;
    --no-start) START=0; shift ;;
    --uninstall) UNINSTALL=1; shift ;;
    *) die "unknown option $1" ;;
  esac
done

if [ "$UNINSTALL" = 1 ]; then
  if [ -f "$AGENT" ]; then
    launchctl bootout "gui/$(id -u)" "$AGENT" 2>/dev/null || true
    rm -f "$AGENT"
  fi
  rm -rf "$APP" "$BIN" "$TOOL_DIR"
  rm -f "$STEAM_BIN/steam_dev.cfg"
  say "Removed. Game prefixes stay in steamapps/compatdata/<appid>/pfx; restart Steam."
  exit 0
fi

command -v node >/dev/null || die "node is needed (brew install node)"
[ -d "$STEAM_BIN" ] || die "start Mac Steam once and log in first"

# Compat tool
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
  cp "$DIST/neutron" "$DIST/compatibilitytool.vdf" "$DIST/toolmanifest.vdf" "$TOOL_DIR/"
fi
[ -f "$TOOL_DIR/neutron.env" ] || printf '# KEY=value lines for all games, e.g. NEUTRON_HUD=1\n' > "$TOOL_DIR/neutron.env"

# Start scripts, independent of where this repo lives
say "Installing the start scripts"
mkdir -p "$BIN"
cp "$ROOT/dev/steam.sh" "$ROOT/dev/steamjs.mjs" "$ROOT"/dev/steam-*.js "$BIN/"
chmod +x "$BIN/steam.sh"
NODE_DIR="$(dirname "$(command -v node)")"
cat > "$BIN/start" <<EOF
#!/bin/bash
# Starts Mac Steam with neutron (installed by neutron's install.sh).
export PATH="$NODE_DIR:/usr/bin:/bin:/usr/sbin:/sbin"
exec "$BIN/steam.sh" "\$@" >> "$LOGS/steam-start.log" 2>&1
EOF
chmod +x "$BIN/start"

# Dock launcher
say "Installing $APP"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cat > "$APP/Contents/MacOS/launcher" <<EOF
#!/bin/bash
exec "$BIN/start"
EOF
chmod +x "$APP/Contents/MacOS/launcher"
icon=""
for i in /Applications/Steam.app/Contents/Resources/*.icns; do [ -f "$i" ] && { icon="$i"; break; }; done
[ -n "$icon" ] && cp "$icon" "$APP/Contents/Resources/AppIcon.icns"
cat > "$APP/Contents/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>launcher</string>
    <key>CFBundleIdentifier</key><string>net.lixsl.neutron.steam</string>
    <key>CFBundleName</key><string>Steam (Neutron)</string>
    <key>CFBundleIconFile</key><string>AppIcon</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>1.0</string>
    <key>LSUIElement</key><true/>
</dict>
</plist>
EOF

# Login item
if [ "$LOGIN" = 1 ]; then
  say "Installing the login item"
  mkdir -p "$(dirname "$AGENT")"
  cat > "$AGENT" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>net.lixsl.neutron.steam</string>
    <key>ProgramArguments</key><array><string>$BIN/start</string></array>
    <key>RunAtLoad</key><true/>
    <key>LimitLoadToSessionType</key><string>Aqua</string>
</dict>
</plist>
EOF
  say "Turn off Steam's own 'Run Steam when my computer starts' (Steam > Settings > Interface)"
fi

if [ "$START" = 1 ]; then
  say "Starting Steam with neutron (maps all games without a Mac version)"
  "$BIN/steam.sh" --map-all
fi
say "Done. Start Steam with \"Steam (Neutron)\" from now on."
