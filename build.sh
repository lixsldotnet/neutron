#!/usr/bin/env bash
#
# Builds the neutron runtime for Apple Silicon: arm64 Wine with ARM64EC
# system DLLs, FEX as the x86_64 emulator (xtajit64 role), DXMT for D3D10/11/12
# on Metal and Proton's lsteamclient bridged to the Mac steamclient.dylib.
# Nothing runs under Rosetta; only the game's own x86_64 code is translated.
#
# Output: <work>/dist/neutron, a compat tool folder (dev/steam.sh --dist).
# Steps are skipped when their output exists; delete a folder to redo a step.
# Takes a while on the first run (Wine, LLVM 15 for DXMT, FEX).
#
# Usage: ./build.sh [work-dir]     (default: ./.native-build)
# Needs: Xcode, Homebrew: bison flex autoconf cmake ninja meson pkgconf gnutls freetype
#
set -euo pipefail

WINE_VERSION="11.19"
PROTON_REF="5b89db940e0ebe3a137a6009a3589232fe084c09"   # ValveSoftware/Proton proton_11.0
FEX_REF="14c92681f4d62cf84d901460e0358de09c8847a7"      # FEX-Emu/FEX FEX-2610
DXMT_REF="e94c312f5c054263acf261cfa109edf13e757587"     # 3Shain/dxmt main 2026-10-06
LLVM_VERSION="llvmorg-15.0.7"                           # DXMT airconv needs LLVM 15
LLVM_MINGW_VERSION="20260908"
LLVM_MINGW_DIR="llvm-mingw-$LLVM_MINGW_VERSION-ucrt-macos-universal"
MINGW_W64_REF="9f55f4a2d3b9783dab76d9808be3d6279b99ec2a" # the CRT llvm-mingw 20260908 was built from
MONO_VERSION="11.3.0"                                   # MONO_VERSION in Wine's dlls/appwiz.cpl/addons.c

# The TEB lives in a pthread TSD slot (macOS clears x18), Wine publishes the slot
# offset next to KUSER_SHARED_DATA, which has to sit above the 4 GB __PAGEZERO.
NATIVE_DEFS="-DWINE_TEB_IN_TSD -DWINE_USER_SHARED_DATA_VA=0x7ffffdfe0000ull"

export MACOSX_DEPLOYMENT_TARGET="15.0"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PATCHES="$ROOT/patches"
WORK="${1:-$PWD/.native-build}"
T="$WORK/toolchains"
SRC="$WORK/src"
JOBS="$(sysctl -n hw.ncpu)"
LM="$T/$LLVM_MINGW_DIR"

say() { printf '\033[36m> %s\033[0m\n' "$*"; }
die() { printf '\033[31mx %s\033[0m\n' "$*" >&2; exit 1; }

[ "$(uname -m)" = "arm64" ] || die "Apple Silicon only."
for tool in autoreconf cmake ninja meson pkgconf git curl; do
  command -v "$tool" >/dev/null || die "missing $tool (brew install autoconf cmake ninja meson pkgconf)"
done
for lib in gnutls freetype; do
  [ -d "$(brew --prefix "$lib" 2>/dev/null)/lib" ] || die "missing $lib (brew install $lib)"
done

mkdir -p "$T" "$SRC"
export PATH="$LM/bin:$(brew --prefix bison)/bin:$(brew --prefix flex)/bin:$PATH"
export PKG_CONFIG_PATH="$(brew --prefix)/lib/pkgconfig"

# git checkout of one commit, plus patches
fetch_git() {  # fetch_git <dir> <url> <ref> [patch...]
  local dir="$1" url="$2" ref="$3"; shift 3
  [ -d "$dir" ] && return 0
  say "Fetching $(basename "$dir") ${ref:0:12}"
  git init -q "$dir"
  git -C "$dir" remote add origin "$url"
  git -C "$dir" fetch -q --depth 1 origin "$ref"
  git -C "$dir" checkout -q FETCH_HEAD
  git -C "$dir" submodule update -q --init --depth 1
  for p in "$@"; do
    say "Applying $(basename "$p")"
    git -C "$dir" apply "$p"
  done
}

#-------------------------------------------------------------------------------
#  toolchain: llvm-mingw with NtCurrentTeb from the TSD slot
#-------------------------------------------------------------------------------

if [ ! -d "$LM" ]; then
  say "Fetching llvm-mingw $LLVM_MINGW_VERSION"
  curl -sfL "https://github.com/mstorsjo/llvm-mingw/releases/download/$LLVM_MINGW_VERSION/$LLVM_MINGW_DIR.tar.xz" \
    | tar xJ -C "$T"
  (cd "$LM" && patch -p1 -s < "$PATCHES/llvm-mingw/0001-ntcurrentteb-tsd.patch")

  # The prebuilt CRT startup objects read the TEB through x18: rebuild them with
  # the patched header, as ARM64X like llvm-mingw does (--enable-arm64x, cfguard).
  fetch_git "$SRC/mingw-w64" https://git.code.sf.net/p/mingw-w64/mingw-w64 "$MINGW_W64_REF"
  say "Rebuilding the CRT startup objects"
  crt="$WORK/crt-objs"
  rm -rf "$crt"; mkdir -p "$crt"
  cflags=(-marm64x -O2 -pipe -std=gnu99 -fno-builtin -D_CRTBLD -D_WIN32_WINNT=0x0f00 -D__MSVCRT_VERSION__=0x600
          -D__USE_MINGW_ANSI_STDIO=0 -mguard=cf -DHAS_CFGUARD -I"$SRC/mingw-w64/mingw-w64-crt/include")
  c="$SRC/mingw-w64/mingw-w64-crt/crt"
  aarch64-w64-mingw32-clang "${cflags[@]}" -D_SYSCRT=1 -c "$c/crtexe.c"  -o "$crt/crt1.o"
  aarch64-w64-mingw32-clang "${cflags[@]}" -D_SYSCRT=1 -c "$c/ucrtexe.c" -o "$crt/crt1u.o"
  aarch64-w64-mingw32-clang "${cflags[@]}" -D_SYSCRT=1 -c "$c/crtdll.c"  -o "$crt/dllcrt1.o"
  aarch64-w64-mingw32-clang "${cflags[@]}" -c "$c/tlsdtor.c" -o "$crt/libarm64_libmingw32_a-tlsdtor.o"
  L="$LM/aarch64-w64-mingw32/lib"
  cp "$crt/crt1.o" "$L/crt1.o";       cp "$crt/crt1.o" "$L/crt2.o"
  cp "$crt/crt1u.o" "$L/crt1u.o";     cp "$crt/crt1u.o" "$L/crt2u.o"
  cp "$crt/dllcrt1.o" "$L/dllcrt1.o"; cp "$crt/dllcrt1.o" "$L/dllcrt2.o"
  (cd "$crt" && "$LM/bin/llvm-ar" r "$L/libmingw32.a" libarm64_libmingw32_a-tlsdtor.o)
fi

#-------------------------------------------------------------------------------
#  Wine (arm64 unix side, ARM64EC + aarch64 PE side) with lsteamclient
#-------------------------------------------------------------------------------

W="$SRC/wine-$WINE_VERSION"
if [ ! -d "$W" ]; then
  say "Fetching Wine $WINE_VERSION"
  curl -sfL "https://dl.winehq.org/wine/source/${WINE_VERSION%%.*}.x/wine-$WINE_VERSION.tar.xz" | tar xJ -C "$SRC"
  for p in "$PATCHES"/wine/0*.patch; do
    say "Applying $(basename "$p")"
    (cd "$W" && patch -p1 -s < "$p")
  done

  say "Fetching Proton lsteamclient + steam_helper ($PROTON_REF)"
  rm -rf "$SRC/proton"
  git init -q "$SRC/proton"
  git -C "$SRC/proton" remote add origin https://github.com/ValveSoftware/Proton.git
  git -C "$SRC/proton" sparse-checkout set lsteamclient steam_helper
  git -C "$SRC/proton" fetch -q --depth 1 --filter=blob:none origin "$PROTON_REF"
  git -C "$SRC/proton" checkout -q FETCH_HEAD
  cp -R "$SRC/proton/lsteamclient" "$W/dlls/lsteamclient"
  cp -R "$SRC/proton/steam_helper" "$W/programs/steam_helper"
  (cd "$W/dlls/lsteamclient" && patch -p1 -s < "$PATCHES/proton/0001-lsteamclient-macos.patch" \
                             && patch -p1 -s < "$PATCHES/proton/0002-lsteamclient-link-libcxx.patch")
  (cd "$W/programs/steam_helper" && patch -p1 -s < "$PATCHES/proton/0003-steam-helper-macos.patch")
  (cd "$W" && autoreconf -f > /dev/null 2>&1)
fi

B="$WORK/wine-build"
if [ ! -f "$B/Makefile" ]; then
  say "Configuring Wine (log: $B/configure.log)"
  mkdir -p "$B"
  (cd "$B" && "$W/configure" --enable-archs=arm64ec,aarch64 --without-x --without-gstreamer \
      --without-cups --without-sane --without-gphoto --without-krb5 --without-pcap --without-usb \
      --without-v4l2 --without-pulse --without-capi --without-opencl --without-inotify \
      CFLAGS="-g -O2 $NATIVE_DEFS" CROSSCFLAGS="-g -O2 $NATIVE_DEFS" > configure.log 2>&1) \
    || die "configure failed, see $B/configure.log"
fi
say "Building Wine (log: $B/make.log)"
make -C "$B" -j"$JOBS" > "$B/make.log" 2>&1 || die "Wine build failed, see $B/make.log"

# Headers and tools for DXMT's winemetal (wine_install_path)
SDK="$WORK/wine-sdk"
if [ ! -x "$SDK/bin/winebuild" ]; then
  say "Installing the Wine SDK for DXMT"
  make -C "$B" install prefix="$SDK" -j8 > "$WORK/wine-sdk.log" 2>&1 || die "make install failed"
fi

#-------------------------------------------------------------------------------
#  FEX (ARM64EC emulator DLL + macOS UnixLib)
#-------------------------------------------------------------------------------

fetch_git "$SRC/FEX" https://github.com/FEX-Emu/FEX.git "$FEX_REF" "$PATCHES"/fex/*.patch
if [ ! -f "$WORK/fex-arm64ec/Bin/libarm64ecfex.dll" ]; then
  say "Building FEX ARM64EC (log: $WORK/fex-build.log)"
  cmake -S "$SRC/FEX" -B "$WORK/fex-arm64ec" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$SRC/FEX/Data/CMake/toolchain_mingw.cmake" -DMINGW_TRIPLE=arm64ec-w64-mingw32 \
    -DENABLE_LTO=False -DENABLE_ASSERTIONS=False -DENABLE_JEMALLOC_GLIBC_ALLOC=False -DBUILD_TESTING=False \
    -DTUNE_ARCH=generic -DTUNE_CPU=none -DRANGES_NATIVE=OFF \
    -DCMAKE_C_FLAGS=-DNEUTRON_TEB_IN_TSD -DCMAKE_CXX_FLAGS=-DNEUTRON_TEB_IN_TSD -DCMAKE_ASM_FLAGS=-DNEUTRON_TEB_IN_TSD \
    > "$WORK/fex-build.log" 2>&1
  cmake --build "$WORK/fex-arm64ec" >> "$WORK/fex-build.log" 2>&1 || die "FEX build failed, see $WORK/fex-build.log"
fi
if [ ! -f "$WORK/fex-unixlib/libarm64ecfex.so" ]; then
  say "Building the FEX UnixLib"
  cmake -S "$SRC/FEX/Source/Windows/UnixLib" -B "$WORK/fex-unixlib" -G Ninja -DCMAKE_BUILD_TYPE=Release > /dev/null
  cmake --build "$WORK/fex-unixlib" > /dev/null || die "FEX UnixLib build failed"
fi

#-------------------------------------------------------------------------------
#  DXMT (arm64ec PE + aarch64-unix winemetal.so, LLVM 15 for airconv)
#-------------------------------------------------------------------------------

if [ ! -d "$T/llvm-darwin-arm64" ]; then
  [ -d "$T/llvm-project" ] || git clone -q --depth 1 --branch "$LLVM_VERSION" \
    https://github.com/llvm/llvm-project.git "$T/llvm-project"
  say "Building LLVM 15 for macOS arm64 (log: $T/llvm-darwin-arm64.log)"
  cmake -B "$T/llvm-darwin-arm64-build" -S "$T/llvm-project/llvm" -G Ninja \
    -DCMAKE_INSTALL_PREFIX="$T/llvm-darwin-arm64" -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DLLVM_HOST_TRIPLE=arm64-apple-darwin -DLLVM_ENABLE_ASSERTIONS=Off -DLLVM_ENABLE_ZSTD=Off \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-D_LIBCPP_KEEP_TRANSITIVE_INCLUDES_LLVM23" \
    -DLLVM_TARGETS_TO_BUILD="" -DLLVM_BUILD_TOOLS=Off -DLLVM_INCLUDE_BENCHMARKS=Off \
    -DBUG_REPORT_URL="https://github.com/3Shain/dxmt" -DPACKAGE_VENDOR="DXMT" \
    -DLLVM_VERSION_PRINTER_SHOW_HOST_TARGET_INFO=Off > "$T/llvm-darwin-arm64.log" 2>&1
  { cmake --build "$T/llvm-darwin-arm64-build" && cmake --install "$T/llvm-darwin-arm64-build"; } \
    >> "$T/llvm-darwin-arm64.log" 2>&1 || die "LLVM build failed, see $T/llvm-darwin-arm64.log"
fi

fetch_git "$SRC/dxmt" https://github.com/3Shain/dxmt.git "$DXMT_REF" "$PATCHES"/dxmt/*.patch
if [ ! -f "$WORK/dxmt-install/aarch64-unix/winemetal.so" ]; then
  say "Building DXMT arm64ec (log: $WORK/dxmt-build.log)"
  (cd "$SRC/dxmt" && meson setup --cross-file build-arm64ec.txt -Dnative_llvm_path="$T/llvm-darwin-arm64" \
     -Dwine_install_path="$SDK" -Denable_d3d12=true build-arm64ec --buildtype release \
     --prefix "$WORK/dxmt-install" --strip) > "$WORK/dxmt-build.log" 2>&1
  { meson compile -C "$SRC/dxmt/build-arm64ec" && meson install -C "$SRC/dxmt/build-arm64ec"; } \
    >> "$WORK/dxmt-build.log" 2>&1 || die "DXMT build failed, see $WORK/dxmt-build.log"
fi

#-------------------------------------------------------------------------------
#  dist: compat tool folder
#-------------------------------------------------------------------------------

DIST="$WORK/dist/neutron"
FILES="$DIST/files"
say "Assembling $DIST"
rm -rf "$DIST"
mkdir -p "$DIST"
make -C "$B" install prefix="$FILES" -j8 > "$WORK/dist-install.log" 2>&1 || die "make install failed"
rm -rf "$FILES/include" "$FILES/share/man"

PE="$FILES/lib/wine/aarch64-windows"
UNIX="$FILES/lib/wine/aarch64-unix"
for d in d3d11 dxgi d3d10core d3d12 winemetal; do cp "$WORK/dxmt-install/aarch64-windows/$d.dll" "$PE/"; done
cp "$WORK/dxmt-install/aarch64-unix/winemetal.so" "$UNIX/"
cp "$WORK/fex-arm64ec/Bin/libarm64ecfex.dll" "$PE/"
cp "$WORK/fex-unixlib/libarm64ecfex.so" "$UNIX/"

# The loader again inside an app bundle: Wine starts Windows processes from there
# (wine patches 0003 and 0006), so macOS sees the Info.plist (game category, Game Mode).
mkdir -p "$UNIX/Neutron.app/Contents/MacOS"
cp "$UNIX/wine" "$UNIX/Neutron.app/Contents/MacOS/wine"
cp "$ROOT/tool/Neutron-Info.plist" "$UNIX/Neutron.app/Contents/Info.plist"

# Wine dlopens GnuTLS and FreeType by soname. Bundle them with their Homebrew
# dependencies; the tool puts files/lib first in DYLD_FALLBACK_LIBRARY_PATH,
# which dyld also uses when the absolute install name is missing.
bundle_dylib() {  # bundle_dylib <path>
  local src="$1" name dep
  name="$(basename "$src")"
  [ -e "$FILES/lib/$name" ] && return 0
  cp -L "$src" "$FILES/lib/$name"
  for dep in $(otool -L "$src" | awk 'NR > 1 && $1 ~ /^\/opt\/homebrew\// {print $1}'); do
    bundle_dylib "$dep"
  done
}
bundle_dylib "$(brew --prefix gnutls)/lib/libgnutls.30.dylib"
bundle_dylib "$(brew --prefix freetype)/lib/libfreetype.6.dylib"

# Wine Mono, unpacked into share/wine/mono: Wine uses it from there without the
# download prompt (a prompt nobody sees blocks the first start of a prefix).
MONO_TAR="$WORK/downloads/wine-mono-$MONO_VERSION-arm64.tar.xz"
if [ ! -f "$MONO_TAR" ]; then
  say "Fetching Wine Mono $MONO_VERSION"
  mkdir -p "$WORK/downloads"
  curl -sfL -o "$MONO_TAR" \
    "https://github.com/madewokherd/wine-mono/releases/download/wine-mono-$MONO_VERSION/wine-mono-$MONO_VERSION-arm64.tar.xz"
fi
mkdir -p "$FILES/share/wine/mono"
tar xJf "$MONO_TAR" -C "$FILES/share/wine/mono"

cp "$ROOT/tool/neutron" "$ROOT/tool/toolmanifest.vdf" "$ROOT/tool/compatibilitytool.vdf" "$DIST/"
chmod +x "$DIST/neutron"

say "Done: $DIST ($(du -sh "$DIST" | cut -f1))"
say "Install: dev/steam.sh --dist $DIST"
