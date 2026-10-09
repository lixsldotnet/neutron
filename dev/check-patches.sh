#!/usr/bin/env bash
#
# Applies every patch in patches/ to the pinned upstream sources, the same way
# build.sh does, without building anything. Runs on macOS and Linux (CI).
# The pinned versions are read from build.sh.
#
# Usage: dev/check-patches.sh [work dir]   (default: a temporary dir, removed after)
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PATCHES="$ROOT/patches"

# The version block at the top of build.sh (plain NAME="value" lines).
eval "$(sed -n '/^WINE_VERSION=/,/^MONO_VERSION=/p' "$ROOT/build.sh" | grep -E '^[A-Z_0-9]+=')"

if [ $# -gt 0 ]; then
  WORK="$1"
  mkdir -p "$WORK"
else
  WORK="$(mktemp -d)"
  trap 'rm -rf "$WORK"' EXIT
fi

fail=0
ok()  { printf 'ok    %s\n' "$1"; }
bad() { printf 'FAIL  %s\n' "$1"; fail=1; }
apply_patch() {  # apply_patch <dir> <patch>: patch -p1 like build.sh
  if (cd "$1" && patch -p1 -s --batch < "$2" > "$WORK/patch.log" 2>&1); then ok "$(basename "$2")"
  else bad "$(basename "$2")"; sed 's/^/      /' "$WORK/patch.log"; fi
}
git_apply() {  # git_apply <repo> <patch>: git apply like build.sh's fetch_git
  if git -C "$1" apply "$2" 2> "$WORK/patch.log"; then ok "$(basename "$2")"
  else bad "$(basename "$2")"; sed 's/^/      /' "$WORK/patch.log"; fi
}
fetch_git() {  # fetch_git <dir> <url> <ref>
  git init -q "$1"
  git -C "$1" remote add origin "$2"
  git -C "$1" fetch -q --depth 1 origin "$3"
  git -C "$1" checkout -q FETCH_HEAD
}

echo "== llvm-mingw $LLVM_MINGW_VERSION"
mkdir -p "$WORK/llvm-mingw"
curl -sfL "https://github.com/mstorsjo/llvm-mingw/releases/download/$LLVM_MINGW_VERSION/$LLVM_MINGW_DIR.tar.xz" \
  | tar xJf - -C "$WORK/llvm-mingw" "$LLVM_MINGW_DIR/generic-w64-mingw32/include/winnt.h"
for p in "$PATCHES"/llvm-mingw/*.patch; do apply_patch "$WORK/llvm-mingw/$LLVM_MINGW_DIR" "$p"; done

echo "== Wine $WINE_VERSION"
curl -sfL "https://dl.winehq.org/wine/source/${WINE_VERSION%%.*}.x/wine-$WINE_VERSION.tar.xz" | tar xJf - -C "$WORK"
W="$WORK/wine-$WINE_VERSION"
for p in "$PATCHES"/wine/0*.patch; do apply_patch "$W" "$p"; done

echo "== Proton ${PROTON_REF:0:12} (lsteamclient, steam_helper)"
git init -q "$WORK/proton"
git -C "$WORK/proton" remote add origin https://github.com/ValveSoftware/Proton.git
git -C "$WORK/proton" sparse-checkout set lsteamclient steam_helper
git -C "$WORK/proton" fetch -q --depth 1 --filter=blob:none origin "$PROTON_REF"
git -C "$WORK/proton" checkout -q FETCH_HEAD
cp -R "$WORK/proton/lsteamclient" "$W/dlls/lsteamclient"
cp -R "$WORK/proton/steam_helper" "$W/programs/steam_helper"
apply_patch "$W/dlls/lsteamclient" "$PATCHES/proton/0001-lsteamclient-macos.patch"
apply_patch "$W/dlls/lsteamclient" "$PATCHES/proton/0002-lsteamclient-link-libcxx.patch"
apply_patch "$W/programs/steam_helper" "$PATCHES/proton/0003-steam-helper-macos.patch"

echo "== FEX ${FEX_REF:0:12}"
fetch_git "$WORK/FEX" https://github.com/FEX-Emu/FEX.git "$FEX_REF"
for p in "$PATCHES"/fex/*.patch; do git_apply "$WORK/FEX" "$p"; done

echo "== DXMT ${DXMT_REF:0:12}"
fetch_git "$WORK/dxmt" https://github.com/3Shain/dxmt.git "$DXMT_REF"
for p in "$PATCHES"/dxmt/*.patch; do git_apply "$WORK/dxmt" "$p"; done

[ "$fail" = 0 ] && echo "all patches apply" || echo "some patches do not apply"
exit "$fail"
