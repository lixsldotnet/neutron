# neutron

A Proton-style Steam compat tool for macOS on Apple Silicon.

With neutron the normal Mac Steam client can install and run Windows games, like Proton does on Linux and the Steam Deck. Steam downloads the Windows version of the game and starts it through neutron. Under the hood its arm64 Wine with ARM64EC system DLLs, FEX for the x86_64 code of the game, DXMT for Direct3D on Metal and the Steam bridge from Proton, which talks to the Steam client you already run. There is no second Windows Steam inside Wine and no Rosetta. neutron is my own project, it is not Valve's Proton.

You dont need CrossOver, Apple's Game Porting Toolkit (D3DMetal) or any other paid or closed product for it. Everything is built from source. Wine, FEX, DXMT, LLVM and mingw-w64 are open source and neutron itself is too (LGPL-2.1-or-later). The only exception is the Steam bridge from Proton (lsteamclient), it is under Valve's Steamworks SDK license, so the build downloads it from Valve's Proton repo and this repo only has the patches for it.

## AI usage

I want to be clear about this. Im a C# developer, not a Wine or translation layer developer. I built neutron together with AI (Claude Code from Anthropic). Most of the patches, scripts and docs were written by the AI. I set the direction, tested everything on my Mac with real games and decided what goes in. The numbers and test results in the docs come from real runs, not from guesses.

So please read the patches with that in mind, especially the deep parts like ntdll, FEX, the DXMT shader translation and msync. If you know this stuff and see something wrong, an issue or PR is very welcome.

## Status

Its early. Real games run, but expect problems, only a few games are tested so far. I tested only on macOS 27 with a M5 Max.

| Game | App id | Engine | API | State |
|---|---|---|---|---|
| Abiotic Factor | 427410 | Unreal Engine 5 | D3D11 | plays |
| Gamble With Your Friends | 3892270 | Unity | D3D11, D3D12 | plays, about 100 FPS at 3440x1440 |
| Ready or Not | 1144200 | Unreal Engine 5 | D3D11 | plays, neutron forces DX11 for it |

32-bit games dont work and will not. Their exe has to load below 4 GB and arm64 macOS does not allow anything there, so this is a limit of the design. Anti-cheat (EAC, BattlEye) and the Steam overlay dont work either. D3D12 is limited, Unreal 5 games need `NEUTRON_D3D12_SM6=1` for it and that is still experimental.

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

Wine runs natively as arm64. Its Windows DLLs are built as ARM64EC, so only the game's own code gets emulated and every call into a Windows API runs at native speed. FEX is the x86_64 emulator, Wine loads it in the role xtajit64.dll has on Windows on ARM. DXMT translates Direct3D 10, 11 and 12 to Metal and MetalFX upscales the picture. lsteamclient is the Steam bridge from Proton, ported back to macOS, so login, ownership, achievements and cloud saves come from the Mac Steam you already run.

The Mac Steam client has Steam Play compiled in, it is only hidden. neutron turns it on, then Steam downloads the Windows depots itself and starts the games through the tool.

More details are in [docs/neutron.md](docs/neutron.md).

## Download and install

Every version gets built by GitHub Actions. Download `neutron-macos-arm64.tar.xz` from the newest [release](https://github.com/lixsldotnet/neutron/releases), extract it and run the installer:

```sh
tar xf neutron-macos-arm64.tar.xz
cd neutron
./install.sh
```

You need a Mac with Apple Silicon and macOS 15 or newer (tested only on macOS 27), Mac Steam in `/Applications`, started once and logged in, and Node 22 or newer for the Steam hook (`brew install node`).

## Requirements for building

If you want to build it yourself you need the full Xcode, not only the Command Line Tools, plus the Metal toolchain because DXMT compiles its shaders with it:

```sh
xcodebuild -downloadComponent MetalToolchain
brew install bison flex autoconf cmake ninja meson pkgconf gnutls freetype node
```

Node has to be version 22 or newer, the Steam hook uses its built-in WebSocket. The build needs about 25 GB of free disk space. The first build took 9.5 minutes on my M5 Max, on a smaller Mac it takes longer (LLVM, Wine and FEX are the big ones).

## Build

```sh
./build.sh
```

The script downloads pinned versions of Wine, Proton (lsteamclient, steam_helper), FEX, DXMT, LLVM 15 (for the shader converter of DXMT), llvm-mingw, Wine Mono and SDL (SDL3 with sdl2-compat, for game controllers), applies the patches from `patches/` and builds everything into `.native-build`. The result is the tool folder `.native-build/dist/neutron`. Steps that are done already get skipped, if you want to redo one delete its folder in `.native-build`. With `./build.sh <dir>` it uses another work dir.

## Install and uninstall

```sh
./install.sh                     # installs the download (dist/neutron) or your build (.native-build/dist/neutron)
./install.sh --dist <dir>        # installs another build
./install.sh --uninstall         # puts Steam.app back and removes neutron
```

`install.sh` quits Steam, installs the tool into `~/Library/Application Support/neutron`, changes `/Applications/Steam.app` (see below) and starts Steam again. After that Windows games have a Install button in Mac Steam and start like any other game.

`--uninstall` puts Valve's files back into Steam.app and removes the tool and `steam_dev.cfg`. The game prefixes in `steamapps/compatdata/<appid>/pfx` stay. It leaves the games mapped to `neutron_proton` in the Steam config on purpose, because when you set a installed game without Mac version to "no compat tool", Steam deletes its files.

## What neutron changes in Steam

The Mac client only turns on Steam Play when it starts with some special settings, so neutron hooks into every Steam start (Dock, login item, Steam's own restart).

`install.sh` points `CFBundleExecutable` of Steam.app to `Contents/MacOS/steam_neutron`. This is a small script that turns Steam Play on, starts the hook (`dev/steam-hook.sh`, log in `~/Library/Logs/neutron/steam-hook.log`) and then runs Valve's unchanged `steam_osx`.

Valve's `steam_osx` is bound to the `Info.plist` of the bundle, so after the change `steam_osx` and the bundle get signed ad hoc. That means Steam.app has no Valve signature anymore. Valve's files are backed up first and `./install.sh --uninstall` puts them back.

The start script writes `steam_dev.cfg` (platform `linux`, this turns on the compat layer) into Steam's client folder at every start. When Steam is up the hook sets the platform back to macOS, so Mac games keep their Mac versions.

Steam runs with its CEF remote debugging port on localhost, port 8080 (`./install.sh --cef-port <port>` takes another one). The hook needs it to map games and to add the Compatibility tab. Please know that any local program can use this port to run JavaScript in the Steam UI with your logged-in account. If some other program already uses the port, the hook cant reach Steam.

A Steam client update can replace Steam.app, just run `./install.sh` again after one.

## Security

Wine is not a sandbox. Code a game runs, also exploit code from somebody attacking you in a old multiplayer game, runs as you. It can read your home folder (the prefix maps `Z:` to `/`), start Mac programs and use the network. And over the debug port from above any local program, so also a broken game, can run code in the Steam UI.

With `NEUTRON_SANDBOX=1` every Wine process of the game runs in a macOS sandbox profile (`tool/neutron.sb`). Its opt-in and still needs tests with real games. The game then cant read your home folder except what it needs (so no Documents, Desktop, Downloads, `~/.ssh`, keychain folders or data of other apps), cant read Steam's `config/` and `local.vdf`, cant start Mac programs (`open`, shells, AppleScript, launchd jobs) and cant reach Steam's debug port, its `steam://` pipe or other Unix sockets. It still gets the runtime, the game folder, its prefix, the logs, network, Metal, audio, controllers and what the Steam client library needs (Steam IPC and the game's Steam Cloud folder).

In the sandbox Documents, Desktop and the other user folders of the game become real folders in the prefix, like with Proton. Saves a game wrote into `~/Documents` before stay there and the game dont see them anymore. Links a game opens in the browser and `steam://` links dont work. The sandbox costs about 5 ms more per Wine start, at run time i could not measure anything.

`tests/sandbox/sbtest.sh` checks all of it headless and `tests/sandbox/sandbox-denials.sh` shows what the sandbox denied while a game ran.

## Usage

Games without a Mac version get mapped to neutron automatically, at every Steam start and every 30 seconds while Steam runs. If you set a game back to none it stays none. Mac games are not touched. Installed neutron games get the Steam setting "only update this game when I launch it", because Steam would otherwise delete and download them again at every start (see docs). They still get their updates at every Steam start, the hook starts them.

The Properties dialog of every game has a Compatibility tab (the hook adds it) where you pick the compat tool for the game (Neutron or none) and the neutron settings.

Careful: if you set a installed game without Mac version to none, Steam deletes its files, because there is no Mac version to keep. Uninstall the game first if thats what you want.

`dev/steam.sh` restarts Steam and waits for the hook. It also takes `--map <appid>`, `--launch <appid>=<options>` and `--global neutron_proton|none`.

## Settings

For one game use the Compatibility tab or put `NEUTRON_<NAME>=<value>` words into the launch options of the game (the tab does the same, Mac Steam does not support `VAR=x %command%`). For all games put `KEY=value` lines into `neutron.env` next to the tool, that is `~/Library/Application Support/neutron/compatibilitytools.d/neutron_proton/neutron.env`. Launch options win over `neutron.env`. Changes work from the next game start.

| Setting | Default | What |
|---|---|---|
| `NEUTRON_HUD` | off | `1`: Metal performance HUD (FPS, frame and GPU time). `2`: also DXMT's statistics per frame (costs CPU time). In the tab. |
| `NEUTRON_RENDER_SCALE` | `0.85` | Games see a smaller display and render less pixels, MetalFX scales it back to native size. `1` turns it off. In the tab. |
| `NEUTRON_FEX_TSO` | per game | `strict` gives FEX the full x86 memory ordering, for games with threading bugs. Much slower. `fast` otherwise. In the tab. |
| `NEUTRON_NATIVE_FULLSCREEN` | `1` | Borderless fullscreen games get macOS native fullscreen and Game Mode. In the tab. |
| `NEUTRON_FPS_LOG` | off | `1`: FPS and the slowest frame of every second in the game log. In the tab. |
| `NEUTRON_RAW_MOUSE` | `1` | Mouse look from the device deltas of the mouse (no macOS acceleration). `0`: the cursor deltas of Wine. |
| `NEUTRON_PAD_HIDRAW` | off | `1`: PlayStation and Switch pads reach the game as raw HID devices (like Proton without Steam Input), for games with own support for them (light bar, touchpad, gyro, adaptive triggers, PlayStation buttons in the UI). Then they are no XInput pad anymore, so games that only know XInput dont see them. Off: every pad is a XInput pad. |
| `NEUTRON_PAD_SYSTEM_GESTURES` | off | `1`: the Home/PS and Share button of a pad do what macOS wants again (Launchpad, Game Overlay, screenshot) while the game is in front. Off: the game gets these buttons. |
| `NEUTRON_PRECISE_TIMERS` | `1` | Precise `Sleep()` through kqueue timers. `0` turns it off. |
| `NEUTRON_DLSS` | off | `1`: games see an NVIDIA GPU and can turn on DLSS, which then runs on the MetalFX temporal upscaler (D3D11 and D3D12). Turns the render scale off, DLSS renders smaller itself. Experimental: on an NVIDIA GPU games can take other code paths. In the tab. |
| `NEUTRON_D3D12_SM6` | off | `1`: D3D12 reports shader model 6.6 and feature level 12_0 and DXIL shaders get translated (Unreal 5 games with `-dx12`). Also removes a forced `-dx11` of a game. Experimental. In the tab. |
| `NEUTRON_MSYNC` | off | `1`: Windows events, semaphores and mutexes work inside the game process instead of going through wineserver (30 to 290 times faster). Experimental, set it in `neutron.env`. |
| `NEUTRON_METALFX` | from render scale | MetalFX upscale factor, normally `1 / NEUTRON_RENDER_SCALE`. |
| `NEUTRON_APP_NAME` | game name | Name in the menu bar and Dock. |
| `NEUTRON_LOG` | off | `1`: Wine debug output in the game log. |
| `NEUTRON_SANDBOX` | off | `1`: runs the game in a macOS sandbox, see Security. Set it in `neutron.env` (the helper calls from Steam dont get the launch options). |

The variables for development and debugging are listed in the header of `tool/neutron`.

## Troubleshooting

Every game writes `~/Library/Logs/neutron/neutron-<appid>.log`, the Steam hook (mapping, Compatibility tab) writes `~/Library/Logs/neutron/steam-hook.log`. For more output add `NEUTRON_LOG=1` to the launch options of the game, or set `WINEDEBUG` in `neutron.env` for single Wine channels, for example `WINEDEBUG=err+all,warn+module`.

If games show no Install button or there is no Compatibility tab, look into `steam-hook.log`. After a Steam update run `./install.sh` again.

Every game has its own Wine prefix in `steamapps/compatdata/<appid>/pfx`. When you delete the `compatdata/<appid>` folder the game starts with a fresh prefix, but save games inside it are gone if Steam Cloud does not have them.

For bug reports please use the issue template and attach both logs.

## Repository

| Path | What |
|---|---|
| `build.sh` | Downloads, patches and builds the runtime, puts the tool folder together |
| `install.sh` | Installs the tool and the start script in Steam.app (`--uninstall` reverts) |
| `patches/` | Patches per upstream project: `wine`, `proton`, `fex`, `dxmt`, `llvm-mingw` |
| `tool/neutron` | Entry script of the compat tool (prefix setup, settings, game start) |
| `tool/neutron.sb` | Sandbox profile for `NEUTRON_SANDBOX=1` |
| `tool/peicon.py` | Gets the icon out of the game's exe for its Dock icon |
| `tool/*.vdf`, `tool/Neutron-Info.plist` | Steam tool manifests, Info.plist of the app bundle the games run in |
| `dev/steam-hook.sh` | Runs at every Steam start: Steam Play setup, game mapping, Compatibility tab |
| `dev/steam-sync.js` | Maps games without Mac version, remaps the mapped games after the Steam start |
| `dev/steam-panel.js` | The Compatibility tab in the game properties |
| `dev/steamjs.mjs` | Runs JavaScript in the Steam UI over the CEF debug port |
| `dev/steam.sh` | Restarts Steam and waits for the hook, manual mapping |
| `dev/gametest.sh` | Starts the installed games one after another and measures first frame, FPS and the slowest frame |
| `dev/direct-test.sh` | Shows which swapchain variants macOS presents without compositing |
| `dev/bench/` | Benchmarks and hang watcher ([README](dev/bench/README.md)) |
| `tests/` | Small Windows test programs: Steam bridge, D3D11, D3D12, sync, sandbox ([README](tests/README.md)) |
| `docs/neutron.md` | Architecture, Steam integration, macOS findings, patches, open points |

## Credits

neutron is mostly glue and patches around the work of other people. Big thx to [Wine](https://www.winehq.org), [FEX](https://github.com/FEX-Emu/FEX), [DXMT](https://github.com/3Shain/dxmt), [Proton](https://github.com/ValveSoftware/Proton) (lsteamclient, steam_helper), [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) and [mingw-w64](https://www.mingw-w64.org), [LLVM](https://llvm.org), [Wine Mono](https://github.com/madewokherd/wine-mono), and [GnuTLS](https://www.gnutls.org), [FreeType](https://freetype.org) and [SDL](https://www.libsdl.org) (SDL3 with [sdl2-compat](https://github.com/libsdl-org/sdl2-compat), for game controllers) which are bundled into the runtime. The idea to put settings into the launch options comes from [NotProton](https://github.com/NotProtonNot/NotProton), and [Madeira](https://github.com/willfaust/Madeira) was my reference for running this stack on Darwin.

## License

The patches in `patches/` have the license of the project they patch. Wine and DXMT are LGPL-2.1-or-later, FEX is MIT, steam_helper from Proton is BSD-3-Clause and lsteamclient from Proton is under the Steamworks SDK license. That one is not open source, thats why the build downloads Valve's code and the repo only keeps the patches.

The own files of the repo (scripts, the tool, tests, docs) are under the GNU Lesser General Public License 2.1 or later, see `LICENSE`. If you change neutron and give it to others, you have to publish your changes under the same license.

The download contains the built parts of all these projects, also Valve's lsteamclient, like every Proton release does. `NOTICES.md` lists every part with its license and source, and the release notes name the commit the download was built from.

## Contributing

Help is very welcome: bug reports with logs, tests of games, fixes. How it works (feature branch, pull request to `main`) and the patch conventions are in `CONTRIBUTING.md`.

## Disclaimer

neutron is not affiliated with or endorsed by Valve, Apple, CodeWeavers, WineHQ or the FEX project. Steam is a trademark of Valve Corporation. Use it at your own risk: neutron changes the Steam app bundle and runs games in a environment their makers did not test.
