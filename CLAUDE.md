# CLAUDE.md

Notes for working on this repo (also meant for contributors).

## Project overview

neutron: a Proton-style Steam compat tool for macOS on Apple Silicon. The
native Mac Steam client runs Windows games through it: arm64 Wine 11.19 with
ARM64EC system DLLs, FEX translating only the game's x86_64 code, DXMT for
Direct3D 10/11/12 on Metal, Proton's `lsteamclient` bridging the Steam API to
the Mac `steamclient.dylib`. No second Windows Steam, no Rosetta. Reference
documentation: `docs/neutron.md`. User documentation: `README.md`.

## Build and install

```sh
./build.sh [work-dir]    # default .native-build in the repo, tool folder in <work>/dist/neutron
./install.sh             # installs <work>/dist/neutron, patches Steam.app's start (--uninstall reverts)
./install.sh --dev       # links tool/neutron and the runtime instead of copying them
```

Needs full Xcode with the Metal toolchain, Homebrew `bison flex autoconf cmake
ninja meson pkgconf gnutls freetype node` (node 22+). `build.sh` checks all of
them. Build steps are skipped when their output exists; delete a folder in the
work dir to redo a step.

## Dev loop

- Work dir (`.native-build`): `src/wine-11.19` (patched Wine with Proton's
  `lsteamclient` and `steam_helper`), `src/FEX`, `src/dxmt`, `src/mingw-w64`,
  build dirs `wine-build`, `fex-arm64ec`, `fex-unixlib`, `dxmt-install`,
  toolchains in `toolchains/`, the tool folder in `dist/neutron`.
- `./install.sh --dev` links `tool/neutron` from the repo and the runtime from
  `.native-build/dist/neutron/files` (or a hand-built Wine install in
  `.native-build/wine-install`). Script changes apply at the next game start.
- After changing a component, rebuild that part and copy the result into the
  runtime's `files`. Delete the old file first: never `cp` over a loaded Mach-O
  (macOS kills the process with "Code Signature Invalid").
- `dev/steam.sh` restarts Steam and waits for the hook (also `--map`,
  `--launch`, `--global`). `dev/steamjs.mjs '<expr>'` evaluates JS in Steam's UI.
- Tests: `tests/README.md`. Benchmarks: `dev/bench/README.md`.
  `dev/direct-test.sh` shows which swapchain variants macOS presents Direct.
- Logs: `~/Library/Logs/neutron/neutron-<appid>.log` (per game) and
  `steam-hook.log`. `NEUTRON_LOG=1` for Wine debug output, `NEUTRON_FPS_LOG=1`
  for frame rates.

## Layout

| Path | Purpose |
|------|---------|
| `build.sh` | Fetch pinned upstreams, patch, build, assemble the tool folder |
| `install.sh` | Install the tool, helper scripts and the start script in Steam.app; `--uninstall` |
| `patches/` | Per upstream: `wine`, `proton`, `fex`, `dxmt`, `llvm-mingw` |
| `tool/neutron` | Compat tool entry script: prefix setup, settings, per-game fixes, game start |
| `tool/peicon.py` | Extracts the icon of a game's exe for its app bundle |
| `tool/*.vdf`, `tool/Neutron-Info.plist` | Steam tool manifests, Info.plist of the loader app bundle |
| `dev/steam-hook.sh` | Runs at every Steam start: platform back to macOS, game mapping, Compatibility tab, re-injects after UI reloads |
| `dev/steam-sync.js` | Injected by the hook: remaps mapped games, maps new games without a Mac version |
| `dev/steam-panel.js` | Injected by the hook: Compatibility tab with the neutron settings |
| `dev/steamjs.mjs` | Evaluates JS in Steam's SharedJSContext over the CEF debug port |
| `dev/steam.sh` | Restarts Steam and waits for the hook; manual mapping and launch options |
| `dev/direct-test.sh` | Direct vs Composited presentation check with `tests/d3d11_present.c` |
| `dev/bench/` | Benchmark suite and hang watcher |
| `tests/` | Windows test programs: Steam bridge, D3D11, D3D12 |
| `docs/neutron.md` | Architecture, Steam integration, macOS findings, patches, open points |

## Workflow

- `main` is protected: no direct pushes, no force pushes. Every change goes on a
  feature branch (`feat/...`, `fix/...`, `perf/...`, `docs/...`) and into `main`
  through a pull request on GitHub (`gh pr create`), merged as a squash commit.
- Commits carry a `Signed-off-by:` line (`git commit -s`, see CONTRIBUTING.md).
- The repo is LGPL-2.1-or-later; patches keep the license of what they patch.

## Conventions

- Patches are unified diffs. Mark code changes with a `/* neutron: ... */`
  comment (only in upstream code, not in the repo's own scripts).
  - `wine`: applied with `patch -p1` in the Wine tree, in order. Each patch is
    the full diff of a fixed set of files against a pristine Wine 11.19, and each
    file belongs to one patch only. Regenerate the patch from a pristine copy per
    file, never hand-edit hunks. A change in a file that already belongs to a
    patch goes into that patch.
  - `dxmt`, `fex`: applied with `git apply` in order on the pinned commit.
    A new change is a new numbered patch on top.
  - `proton`: `patch -p1` in `dlls/lsteamclient` and `programs/steam_helper`.
  - `llvm-mingw`: `patch -p1` in the toolchain folder.
- Shell: bash 3.2 compatible (macOS `/bin/bash`), `set -euo pipefail`
  (`tool/neutron` uses `set -uo pipefail` on purpose), quote paths (Steam paths
  contain spaces).
- Valve's lsteamclient is under the Steamworks SDK license: keep only patches
  in the repo, fetch Valve's code at build time. No Apple or Valve binaries in
  the repo.
- Settings: `NEUTRON_*` environment variables, listed in the header of
  `tool/neutron`. Per-game fixes live in `tool/neutron` (`APP_TSO_<appid>`,
  `APP_ARGS_<appid>`); user settings come from the launch options or
  `neutron.env` next to the tool.
- Text: plain technical English, no em dashes, no emojis.
