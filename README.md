# neutron

A Proton-style Steam compat tool for macOS on Apple Silicon.

neutron lets the native Mac Steam client install and run Windows games, the
way Proton does on Linux and the Steam Deck. Steam downloads the Windows
version of a game and starts it through neutron: arm64 Wine with ARM64EC system
DLLs, FEX for the game's x86_64 code, DXMT for Direct3D on Metal, and Proton's
Steam bridge to the Steam client that is already running. There is no second
Windows Steam inside Wine and no Rosetta. neutron is an independent project and
not Valve's Proton.

## Status

Early. Real games run, but expect problems, and only a few games were tested.
It was tested only on macOS 27 with an Apple Silicon M5 Max.

| Game | App id | Engine | API | State |
|---|---|---|---|---|
| Abiotic Factor | 427410 | Unreal Engine 5 | D3D11 | plays; long black screen while loading a map |
| Gamble With Your Friends | 3892270 | Unity | D3D11, D3D12 | plays, about 100 FPS at 3440x1440 on M5 Max |
| Ready or Not | 1144200 | Unreal Engine | D3D11 | plays; neutron forces DX11 for it |

Does not work:

- 32-bit games. Their image has to load below 4 GB, which arm64 macOS does not
  allow. This is a limit of the design, not a missing feature.
- Anti-cheat (EAC, BattlEye) and the Steam overlay.
- D3D12 is limited. neutron forces D3D11 for games where D3D12 fails (Ready or
  Not).

## How it works

```
Mac Steam  ->  neutron  ->  Wine (arm64 + ARM64EC)  ->  Windows game (x86_64)
                              |                           |
                              |                           +- FEX translates the game's x86_64 code
                              |                           +- DXMT runs Direct3D 10/11/12 on Metal
                              |
                              +- lsteamclient (from Proton) bridges the game's Steam API
                                 to the steamclient.dylib of the running Mac Steam
```

- **Wine** runs natively as arm64. Its Windows DLLs are built as ARM64EC, so
  only the game's own code is emulated and calls into Windows APIs run at
  native speed.
- **FEX** is the x86_64 emulator, loaded by Wine in the role `xtajit64.dll` has
  on Windows on ARM.
- **DXMT** translates Direct3D 10, 11 and 12 to Metal. MetalFX upscales the
  output.
- **lsteamclient** is Proton's Steam bridge, ported back to macOS. Login,
  ownership, achievements and cloud saves come from the Mac Steam you already
  run.
- The Mac Steam client has Steam Play compiled in, only hidden. neutron turns it
  on, so Steam downloads the Windows depots itself and starts games through the
  tool.

Details: [docs/neutron.md](docs/neutron.md).

## Requirements

- A Mac with Apple Silicon, macOS 15 or newer (tested on macOS 27 only)
- Mac Steam in `/Applications`, started once and logged in
- Full Xcode (not only the Command Line Tools) with the Metal toolchain, which
  DXMT needs to compile its shaders:
  `xcodebuild -downloadComponent MetalToolchain`
- Homebrew packages:
  `brew install bison flex autoconf cmake ninja meson pkgconf gnutls freetype node`
- Node 22 or newer (the Steam hook uses its built-in WebSocket)
- About 25 GB of free disk space for the build. The first build took 9.5 minutes on an
  M5 Max; expect longer on smaller Macs (LLVM, Wine and FEX take most of it).

## Build

```sh
./build.sh
```

The script fetches pinned versions of Wine, Proton (lsteamclient,
steam_helper), FEX, DXMT, LLVM 15 (for DXMT's shader converter), llvm-mingw and
Wine Mono, applies `patches/` and builds everything into `.native-build`. The
result is the tool folder `.native-build/dist/neutron`. Steps are skipped when
their output exists; delete a folder in `.native-build` to redo a step.
`./build.sh <dir>` uses another work dir.

## Install and uninstall

```sh
./install.sh                     # installs .native-build/dist/neutron
./install.sh --dist <dir>        # installs another build
./install.sh --uninstall         # puts Steam.app back and removes neutron
```

`install.sh` quits Steam, installs the tool into
`~/Library/Application Support/neutron`, changes `/Applications/Steam.app` (see
below) and starts Steam again. From then on, Windows games show an Install
button in Mac Steam and start like any other game.

`--uninstall` restores Valve's files in Steam.app, removes the tool and
`steam_dev.cfg`, and keeps the game prefixes in
`steamapps/compatdata/<appid>/pfx`. It leaves the games mapped to
`neutron_proton` in Steam's config on purpose: when an installed game without a
Mac version is set to no compat tool, Steam deletes its files.

## What neutron changes in Steam

The Mac client only enables Steam Play when it starts up with certain settings,
so neutron hooks into every Steam start (Dock, login item, Steam's own restart):

- **Start script in Steam.app.** The bundle's `CFBundleExecutable` points to
  `Contents/MacOS/steam_neutron`, a small script that turns Steam Play on, starts
  the hook (`dev/steam-hook.sh`, log in `~/Library/Logs/neutron/steam-hook.log`)
  and then runs Valve's unchanged `steam_osx`.
- **Signature.** Valve's `steam_osx` is bound to the bundle's `Info.plist`, so
  `steam_osx` and the bundle are signed ad hoc after the change. Steam.app no
  longer carries Valve's signature. Valve's files are backed up first,
  `./install.sh --uninstall` restores them.
- **steam_dev.cfg.** The start script writes `steam_dev.cfg` (platform `linux`,
  which enables the compat layer) into Steam's client folder at every start. The
  hook sets the platform back to macOS once Steam is up, so Mac games keep
  their Mac versions.
- **CEF debug port.** Steam runs with its CEF remote debugging port on
  localhost, port 8080 (`./install.sh --cef-port <port>` picks another one). The
  hook uses it to map games and add the Compatibility tab. Any local process can
  use this port to run JavaScript in Steam's UI with your logged-in account. If
  another program already uses the port, the hook cannot reach Steam.
- **Steam updates.** A Steam client update can replace Steam.app. Run
  `./install.sh` again after one.

## Security

Wine is not a sandbox. Code that a game runs, including exploit code from an
attacker in an old multiplayer game, runs as you: it can read your home folder
(the prefix maps `Z:` to `/`), start Mac programs and use the network. Steam's
debug port (see above) lets any local process, a compromised game too, run code
in Steam's UI.

`NEUTRON_SANDBOX=1` (opt-in, still being tested with real games) runs every Wine
process of the game under a macOS sandbox profile (`tool/neutron.sb`):

- Denied: your home folder except what the game needs (Documents, Desktop,
  Downloads, `~/.ssh`, keychain folders, other apps' data), Steam's `config/`
  and `local.vdf`, starting Mac programs (`open`, shells, AppleScript, launchd
  jobs), Steam's debug port and its `steam://` pipe, other Unix sockets.
- Allowed: the runtime, the game folder, the game's prefix, logs, network, Metal,
  audio, controllers, and what Steam's client library needs (Steam IPC, the
  game's Steam Cloud folder).
- The game's Documents, Desktop and other user folders become real folders in
  the prefix (like Proton); saves a game wrote into `~/Documents` before stay
  there and are no longer visible to it. Links a game opens in the browser and
  `steam://` links do not work.
- Cost: about 5 ms more per Wine start, nothing measurable at run time.

`tests/sandbox/sbtest.sh` checks all of this headless; `tests/sandbox/sandbox-denials.sh`
shows what the sandbox denied while a game ran.

## Usage

Games without a Mac version are mapped to neutron automatically, at every Steam
start and every 30 seconds while Steam runs. A game you set back to none stays
none. Mac games are left alone.

Each game's Properties dialog has a **Compatibility** tab (added by the hook)
with the compat tool for the game (Neutron or none) and the neutron settings.

Setting an installed game without a Mac version to none makes Steam delete its
files, because there is no Mac version to keep. Uninstall the game first if
that is what you want.

`dev/steam.sh` restarts Steam and waits for the hook. It also takes
`--map <appid>`, `--launch <appid>=<options>` and `--global neutron_proton|none`.

## Settings

Per game, use the Compatibility tab, or put `NEUTRON_<NAME>=<value>` words into
the game's launch options (the tab does the same; Mac Steam does not support
`VAR=x %command%`). For all games, put `KEY=value` lines into `neutron.env` next
to the tool:
`~/Library/Application Support/neutron/compatibilitytools.d/neutron_proton/neutron.env`.
Launch options override `neutron.env`. Changes apply at the next game start.

| Setting | Default | What |
|---|---|---|
| `NEUTRON_HUD` | off | `1`: Metal performance HUD (FPS, frame and GPU time). `2`: adds DXMT's per-frame statistics (costs CPU time). In the tab. |
| `NEUTRON_RENDER_SCALE` | `0.85` | Games see a smaller display and render fewer pixels, MetalFX scales back to native size. `1` turns it off. In the tab. |
| `NEUTRON_FEX_TSO` | per game | `strict` gives FEX the full x86 memory ordering, for games with threading bugs. Much slower. `fast` otherwise. In the tab. |
| `NEUTRON_NATIVE_FULLSCREEN` | `1` | Borderless fullscreen games get macOS native fullscreen and Game Mode. In the tab. |
| `NEUTRON_FPS_LOG` | off | `1`: FPS and the slowest frame per second in the game log. In the tab. |
| `NEUTRON_RAW_MOUSE` | `1` | Mouse look from the mouse's device deltas (no macOS acceleration). `0`: Wine's cursor deltas. |
| `NEUTRON_PRECISE_TIMERS` | `1` | Precise `Sleep()` through kqueue timers. `0` turns it off. |
| `NEUTRON_MSYNC` | off | `1`: Windows events, semaphores and mutexes work in the game process instead of through wineserver (30 to 290 times faster sync calls). Experimental, set it in `neutron.env`. |
| `NEUTRON_METALFX` | from render scale | MetalFX upscale factor, normally `1 / NEUTRON_RENDER_SCALE`. |
| `NEUTRON_APP_NAME` | game name | Name in the menu bar and Dock. |
| `NEUTRON_LOG` | off | `1`: Wine debug output in the game log. |
| `NEUTRON_SANDBOX` | off | `1`: run the game in a macOS sandbox, see Security. Set it in `neutron.env` (Steam's helper calls do not carry the launch options). |

`tool/neutron` lists the development and diagnostic variables in its header.

## Troubleshooting

- **Logs.** Each game writes `~/Library/Logs/neutron/neutron-<appid>.log`. The
  Steam hook (mapping, Compatibility tab) writes
  `~/Library/Logs/neutron/steam-hook.log`.
- **More detail.** Add `NEUTRON_LOG=1` to the game's launch options. For specific
  Wine channels, set `WINEDEBUG` in `neutron.env` (for example
  `WINEDEBUG=err+all,warn+module`).
- **Games show no Install button or no Compatibility tab.** Check
  `steam-hook.log`. After a Steam update, run `./install.sh` again.
- **Prefix.** Each game has its own Wine prefix in
  `steamapps/compatdata/<appid>/pfx`. Deleting the `compatdata/<appid>` folder
  starts the game with a fresh prefix (save games inside it are lost unless
  Steam Cloud has them).
- **Bug reports.** Please use the issue template and attach both logs.

## Repository

| Path | What |
|---|---|
| `build.sh` | Fetches, patches and builds the runtime, assembles the tool folder |
| `install.sh` | Installs the tool and the start script in Steam.app (`--uninstall` reverts) |
| `patches/` | Patches per upstream project: `wine`, `proton`, `fex`, `dxmt`, `llvm-mingw` |
| `tool/neutron` | The compat tool's entry script (prefix setup, settings, game start) |
| `tool/peicon.py` | Extracts the icon of a game's exe for its Dock icon |
| `tool/*.vdf`, `tool/Neutron-Info.plist` | Steam tool manifests, Info.plist of the app bundle games run in |
| `dev/steam-hook.sh` | Runs at every Steam start: Steam Play setup, game mapping, Compatibility tab |
| `dev/steam-sync.js` | Maps games without a Mac version, remaps mapped games after Steam starts |
| `dev/steam-panel.js` | The Compatibility tab in the game properties |
| `dev/steamjs.mjs` | Runs JavaScript in Steam's UI over the CEF debug port |
| `dev/steam.sh` | Restarts Steam and waits for the hook; manual mapping |
| `dev/direct-test.sh` | Shows which swapchain variants macOS presents without compositing |
| `dev/bench/` | Benchmark suite and hang watcher ([README](dev/bench/README.md)) |
| `tests/` | Small Windows test programs: Steam bridge, D3D11, D3D12 ([README](tests/README.md)) |
| `docs/neutron.md` | Architecture, Steam integration, macOS findings, patches, open points |

## Credits

neutron is glue and patches around the work of others:

- [Wine](https://www.winehq.org)
- [FEX](https://github.com/FEX-Emu/FEX)
- [DXMT](https://github.com/3Shain/dxmt)
- [Proton](https://github.com/ValveSoftware/Proton) (lsteamclient, steam_helper)
- [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) and [mingw-w64](https://www.mingw-w64.org)
- [LLVM](https://llvm.org)
- [Wine Mono](https://github.com/madewokherd/wine-mono)
- [GnuTLS](https://www.gnutls.org) and [FreeType](https://freetype.org), bundled into the runtime
- [NotProton](https://github.com/NotProtonNot/NotProton), for the idea of
  settings in the launch options
- [Madeira](https://github.com/willfaust/Madeira), the reference for running
  this stack on Darwin

## License

The patches in `patches/` follow the license of the project they patch:

- Wine and DXMT: LGPL-2.1-or-later
- FEX: MIT
- Proton's lsteamclient: Steamworks SDK license. It is not open source, so the
  build fetches Valve's code and the repo keeps only the patches.
- Proton's steam_helper: BSD-3-Clause

License of the repo's own files: TODO (MIT or LGPL-2.1-or-later)

No binaries are distributed because of lsteamclient's license: build neutron
from source.

## Disclaimer

neutron is not affiliated with or endorsed by Valve, Apple, CodeWeavers,
WineHQ or the FEX project. Steam is a trademark of Valve Corporation. Use at
your own risk: neutron changes the Steam app bundle and runs games in an
environment their makers did not test.
