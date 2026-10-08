#!/usr/bin/env bash
#
# Shows what the neutron game sandbox (NEUTRON_SANDBOX=1, tool/neutron.sb) denied in
# the last minutes, from the macOS unified log ("Sandbox: <proc>(<pid>) deny(1) <op>
# <target>"), grouped and counted. Only Wine processes: wine, wineserver and the
# Windows programs (*.exe, Wine names the processes after them).
#
# Usage: sandbox-denials.sh [minutes] (default 30)
#        sandbox-denials.sh --stream  (live, while a game runs)
set -euo pipefail

PROCS='(wine|wineserver|wine64|env|Neutron|[^ ]+\.exe)'
if [ "${1:-}" = --stream ]; then
  /usr/bin/log stream --style compact --predicate 'sender == "Sandbox"' |
    grep --line-buffered -E "Sandbox: $PROCS\([0-9]+\) deny"
  exit 0
fi
/usr/bin/log show --last "${1:-30}m" --style compact --predicate 'sender == "Sandbox"' |
  grep -E "Sandbox: $PROCS\([0-9]+\) deny" |
  sed -E 's/^.*Sandbox: //; s/\([0-9]+\) deny\([0-9]+\)/:/' |
  sort | uniq -c | sort -rn
