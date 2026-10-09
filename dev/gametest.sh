#!/usr/bin/env bash
#
# Starts installed games through Mac Steam one after another, like a player would
# (steam://rungameid), and measures each start: time to the first frame, the median
# FPS and the slowest frame over a fixed window, crashes and hangs. Then it ends the
# game (its Wine session) and goes to the next one. Needs Steam running with neutron
# installed, a logged-in user and the screen on (games open windows).
#
# The numbers come from DXMT's FPS log (NEUTRON_FPS_LOG=1): the script turns it on in
# neutron.env for the run and puts the file back afterwards. Results are printed as a
# table and appended to .native-build/opt/gametest-history.jsonl.
#
# Usage: dev/gametest.sh [appid...]       (default: every installed game mapped to neutron)
#   GAMETEST_SECONDS   measuring window after the first frame (default 60)
#   GAMETEST_TIMEOUT   max seconds until the first frame (default 240)
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STEAM="$HOME/Library/Application Support/Steam"
APPS="$STEAM/steamapps"
LOGS="$HOME/Library/Logs/neutron"
TOOL_DIR="$HOME/Library/Application Support/neutron/compatibilitytools.d/neutron_proton"
ENV_FILE="$TOOL_DIR/neutron.env"
HISTORY="$REPO/.native-build/opt/gametest-history.jsonl"
SECONDS_WINDOW="${GAMETEST_SECONDS:-60}"
TIMEOUT="${GAMETEST_TIMEOUT:-240}"

die() { printf 'x %s\n' "$*" >&2; exit 1; }
say() { printf '> %s\n' "$*"; }

pgrep -x steam_osx >/dev/null || die "Steam is not running"
[ -d "$TOOL_DIR" ] || die "neutron is not installed ($TOOL_DIR)"

# Installed games mapped to neutron: appmanifest present and a neutron_proton entry in
# config.vdf's CompatToolMapping.
mapped_installed() {
  local id
  for f in "$APPS"/appmanifest_*.acf; do
    id="${f##*_}"; id="${id%.acf}"
    awk -v id="\"$id\"" '
      /"CompatToolMapping"/ { inmap = 1; depth = 0; next }
      !inmap { next }
      /^[ \t]*\{/ { depth++; next }
      /^[ \t]*\}/ { if (--depth <= 0) exit; next }
      depth == 1 { cur = $1 }
      depth == 2 && $1 == "\"name\"" && $2 ~ /^"neutron_proton/ && cur == id { found = 1 }
      END { exit !found }
    ' "$STEAM/config/config.vdf" && echo "$id"
  done
  return 0
}

game_name() {
  awk -F'"' '$2 == "name" { print $4; exit }' "$APPS/appmanifest_$1.acf" 2>/dev/null || echo "$1"
}

ids=("$@")
if [ ${#ids[@]} -eq 0 ]; then
  while IFS= read -r id; do ids+=("$id"); done < <(mapped_installed)
fi
[ ${#ids[@]} -gt 0 ] || die "no installed games mapped to neutron"

# FPS log on for the run, the old neutron.env comes back afterwards.
backup="$(mktemp)"
cp "$ENV_FILE" "$backup" 2>/dev/null || : > "$backup"
restore() { cp "$backup" "$ENV_FILE"; rm -f "$backup"; }
trap restore EXIT
grep -q '^NEUTRON_FPS_LOG=1' "$ENV_FILE" 2>/dev/null || printf 'NEUTRON_FPS_LOG=1\n' >> "$ENV_FILE"

stop_game() {  # stop_game <appid>: end the Wine session of the game's prefix
  local pfx="$APPS/compatdata/$1/pfx" files
  files="$(readlink "$TOOL_DIR/files" 2>/dev/null || echo "$TOOL_DIR/files")"
  WINEPREFIX="$pfx" "$files/bin/wineserver" -k 2>/dev/null || true
  for _ in $(seq 1 30); do
    WINEPREFIX="$pfx" "$files/bin/wineserver" -k0 2>/dev/null || break
    sleep 1
  done
  sleep 3
}

mkdir -p "$(dirname "$HISTORY")"
rows=()
for id in "${ids[@]}"; do
  name="$(game_name "$id")"
  log="$LOGS/neutron-$id.log"
  start_line=$( (wc -l < "$log") 2>/dev/null || echo 0)
  start_line=$((start_line + 0))
  say "$name ($id): starting"
  t0=$(date +%s)
  open "steam://rungameid/$id"

  first="" state="timeout" median="" worst=""
  while [ $(( $(date +%s) - t0 )) -lt "$TIMEOUT" ]; do
    sleep 2
    new="$(tail -n +"$((start_line + 1))" "$log" 2>/dev/null || true)"
    if printf '%s\n' "$new" | grep -q 'neutron: fps'; then first=$(( $(date +%s) - t0 )); state="running"; break; fi
    if printf '%s\n' "$new" | grep -qE '^[0-9-]+ [0-9:]+ exit [0-9]+'; then state="exited before the first frame"; break; fi
  done

  if [ "$state" = running ]; then
    sleep "$SECONDS_WINDOW"
    new="$(tail -n +"$((start_line + 1))" "$log" 2>/dev/null || true)"
    if printf '%s\n' "$new" | grep -qE '^[0-9-]+ [0-9:]+ exit [0-9]+'; then state="crashed while running"; fi
    # Lines "neutron: fps <fps> worst <ms>ms" of the window, the first 5 seconds after
    # the first frame left out (loading).
    read -r median worst < <(printf '%s\n' "$new" | sed -n 's/.*neutron: fps \([0-9.]*\) worst \([0-9.]*\)ms.*/\1 \2/p' \
      | tail -n +6 | sort -n | awk '{ f[NR] = $1; if ($2 > w) w = $2 } END { if (NR) printf "%.1f %.1f\n", f[int((NR + 1) / 2)], w; else print "- -" }')
  fi
  stop_game "$id"

  rows+=("$(printf '%-34s %-9s %-26s %8s %8s %10s' "${name:0:34}" "$id" "$state" "${first:--}" "${median:--}" "${worst:--}")")
  printf '{"time":"%s","appid":%s,"name":"%s","state":"%s","first_frame_s":%s,"fps_median":%s,"worst_ms":%s,"git":"%s"}\n' \
    "$(date '+%F %T')" "$id" "${name//\"/}" "$state" "${first:-null}" "${median:-null}" "${worst:-null}" \
    "$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo none)" | sed 's/:-,/:null,/g; s/:-}/:null}/g' >> "$HISTORY"
done

printf '\n%-34s %-9s %-26s %8s %8s %10s\n' game appid result "first(s)" fps "worst(ms)"
for r in "${rows[@]}"; do printf '%s\n' "$r"; done
printf '\nhistory: %s\n' "$HISTORY"
