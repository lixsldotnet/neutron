# CLAUDE.md

## Project Overview

neutron: Proton for macOS. A Steam compat tool so the native Mac Steam client
runs Windows games: arm64 Wine 11.19 with ARM64EC system DLLs, FEX translating
only the game's x86_64 code, DXMT for Direct3D on Metal, Proton's
`lsteamclient` bridging the Steam API to the Mac `steamclient.dylib`. No second
Windows Steam, no Rosetta. Apple Silicon only. Design and findings:
`docs/neutron.md`.

## Build

```sh
./build.sh [work-dir]   # default ./.native-build, tool folder in <work>/dist/neutron
./install.sh             # or --dev to link the dev tree; Steam (Neutron) app + login item
```

Needs Xcode and Homebrew bison flex autoconf cmake ninja meson pkgconf gnutls
freetype node. Steps are skipped when their output exists.

Dev loop: the work dir keeps the sources (`g0/wine-11.19` with a pristine copy
next to it, `src/FEX`, `src/dxmt`) and build dirs. Rebuild the changed part,
then copy the result into the installed `files` (delete first, never `cp` over
a loaded Mach-O). Logs: `~/Library/Logs/neutron/neutron-<appid>.log`,
`NEUTRON_FPS_LOG=1` for frame rates.

## Layout

| Path | Purpose |
|------|---------|
| `build.sh` | Fetch pinned upstreams, patch, build, assemble the tool folder |
| `install.sh` | Install the tool, start scripts, Steam (Neutron) launcher, login item; `--uninstall` |
| `patches/` | Per upstream: `wine`, `proton`, `fex`, `dxmt`, `llvm-mingw` |
| `tool/` | `neutron` entry script, `compatibilitytool.vdf`, `toolmanifest.vdf`, `Neutron-Info.plist` |
| `dev/` | `steam.sh` (install, Steam Play on, game mapping), `steam-panel.js` (settings panel in Steam), `steamjs.mjs` (Steam JS over CEF) |
| `tests/` | Windows test programs: Steam bridge smoke test, D3D11/D3D12 |
| `docs/neutron.md` | Architecture, Mac client findings, macOS pitfalls, open points |

## Conventions

- Patches: unified diff, applied with `patch -p1` from the project root (Wine,
  FEX, DXMT, llvm-mingw) or the module dir (`dlls/lsteamclient`,
  `programs/steam_helper`). Regenerate from a pristine copy, never hand-edit
  hunks. Mark code changes with a `/* neutron: ... */` comment.
- Shell: bash, `set -euo pipefail`, quote paths (Steam paths contain spaces).
- Valve's lsteamclient is under the Steamworks SDK license: keep only patches
  in the repo, fetch Valve's code at build time. No Apple binaries in the repo.
- Per-game settings live in `tool/neutron` (`APP_TSO_<appid>`,
  `APP_ARGS_<appid>`), user settings in `neutron.env` next to the tool.
