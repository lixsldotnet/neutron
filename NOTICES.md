# Notices

The neutron download contains software built from these projects. Every download
is built by GitHub Actions from one commit of this repository: the release notes
name the commit, `build.sh` in that commit pins the exact upstream versions and
`patches/` holds every change neutron makes to them.

| Part | License | Source |
|---|---|---|
| neutron (scripts, the tool, tests, docs) | LGPL-2.1-or-later | this repository |
| Wine | LGPL-2.1-or-later | https://gitlab.winehq.org/wine/wine |
| FEX | MIT | https://github.com/FEX-Emu/FEX |
| DXMT | LGPL-2.1-or-later | https://github.com/3Shain/dxmt |
| LLVM (linked into DXMT's shader converter) | Apache-2.0 WITH LLVM-exception | https://github.com/llvm/llvm-project |
| Proton steam_helper | BSD-3-Clause | https://github.com/ValveSoftware/Proton |
| Proton lsteamclient | Steamworks SDK license (Valve) | https://github.com/ValveSoftware/Proton |
| Wine Mono | MIT and the licenses of its parts | https://github.com/madewokherd/wine-mono |
| mingw-w64 runtime (in the Windows DLLs) | ZPL-2.1 and public domain | https://www.mingw-w64.org |
| GnuTLS, Nettle, GMP, libtasn1, libidn2, libunistring, p11-kit | LGPL | via Homebrew, see their projects |
| FreeType | FreeType License (FTL) | https://freetype.org |
| SDL3, sdl2-compat (game controllers) | Zlib | https://github.com/libsdl-org/SDL, https://github.com/libsdl-org/sdl2-compat |

The full license texts of Wine, DXMT and neutron: `LICENSE` (LGPL-2.1). The
other license texts are in the source of each project at the pinned version.

lsteamclient is Valve's code under the Steamworks SDK license, the same component
Valve ships in every Proton release. Steam is a trademark of Valve Corporation.
