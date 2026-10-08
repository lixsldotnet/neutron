# neutron

Proton for macOS. neutron is a Steam compatibility tool that lets the native
Mac Steam client install and run Windows games, the same way Proton does on
Linux and the Steam Deck. One Steam, no second Windows Steam inside Wine, no
Rosetta.

Apple Silicon only. Status: early, but real games run (see below).

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
  only the game's own code is emulated. Every call into Windows APIs runs at
  native speed.
- **FEX** is the x86_64 emulator, loaded by Wine like `xtajit64.dll` on
  Windows on ARM.
- **DXMT** translates Direct3D 10, 11 and 12 to Metal. MetalFX upscales the
  output.
- **lsteamclient** is Proton's Steam bridge, ported back to macOS. Login,
  ownership, achievements and cloud saves come from the Mac Steam you already
  run.
- The Mac Steam client has Steam Play compiled in, only hidden. neutron turns it
  on, so Steam downloads the Windows depots itself and starts games through the
  tool.

Details, macOS findings and open points: [docs/neutron.md](docs/neutron.md).

## Tested games

| Game | Engine | API | State |
|---|---|---|---|
| Abiotic Factor | Unreal Engine 5 | D3D11 | plays; long black screen while loading a map |
| Gamble With Your Friends | Unity | D3D11, D3D12 | plays, about 100 FPS at 3440x1440 on M5 Max |

## Requirements

- Mac with Apple Silicon, macOS 15 or newer
- Mac Steam, started once and logged in
- Xcode and Homebrew:
  `brew install bison flex autoconf cmake ninja meson pkgconf gnutls freetype node`

## Build

```sh
./build.sh            # work dir ./.native-build, result in .native-build/dist/neutron
```

The script fetches pinned versions of Wine, Proton (lsteamclient, steam_helper),
FEX, DXMT, LLVM 15 (for DXMT's shader converter), llvm-mingw and Wine Mono,
applies `patches/` and builds everything. The first run takes a while (LLVM
and Wine). Steps are skipped when their output exists; delete a folder in the
work dir to redo a step.

## Install

```sh
./install.sh            # after ./build.sh; --dev uses the dev tree instead of the build
```

This installs the tool into `~/Library/Application Support/neutron`, adds
**Steam (Neutron)** to `~/Applications` and a login item, then starts Steam with
Steam Play turned on and maps every game in your library that has no Mac version
to neutron. After that the Windows games show an Install button in Mac Steam and
start like any other game.

Steam only finds neutron when it is started this way, so start it with
"Steam (Neutron)" (put it in the Dock) and turn off Steam's own "Run Steam when my
computer starts". `./install.sh --uninstall` removes everything again except the
game prefixes.

Under the hood `dev/steam.sh` writes `steam_dev.cfg` (Steam enables its compat
layer), starts Steam with the CEF debug port, switches the platform back to macOS
and adds the settings tab. It also takes `--map <appid>`, `--launch
<appid>=<options>` and `--global neutron_proton|none`.

## Settings

Per game: open the game's Properties in Steam, tab **Compatibility** (added by
`dev/steam.sh` to the running Steam UI, Steam's files are not changed). It picks
the tool for the game (Neutron or none) and has the neutron settings:

| Setting | Default | What |
|---|---|---|
| Performance HUD | off | FPS and frame time, or the full Metal HUD with DXMT's per-frame statistics |
| Render scale | 85% | Games render fewer pixels, MetalFX scales back to native size. 100% turns it off. |
| x86 memory ordering | per game | `Strict` gives FEX the full x86 memory ordering, for games with threading bugs. Much slower. |
| macOS fullscreen and Game Mode | on | Borderless fullscreen games get their own Space and Game Mode |
| FPS log | off | FPS and the slowest frame per second in the log |

The panel stores them as `NEUTRON_<NAME>=<value>` words in the game's launch
options, the tool takes them out again before the game starts. For all games the
same settings go into `neutron.env` next to the tool
(`~/Library/Application Support/neutron/compatibilitytools.d/neutron_proton/neutron.env`),
one `KEY=value` per line, e.g. `NEUTRON_HUD=2`. `NEUTRON_LOG=1` enables Wine debug
output.

Logs: `~/Library/Logs/neutron/neutron-<appid>.log`. Each game gets its own Wine
prefix in `steamapps/compatdata/<appid>/pfx`.

## Not working (yet)

- 32-bit games
- Anti-cheat (EAC, BattlEye) and the Steam overlay
- The compatibility page in Steam's game settings stays hidden; mapping goes
  through `dev/steam.sh`

## Repository

| Path | What |
|---|---|
| `build.sh` | Fetches, patches and builds the runtime, assembles the tool folder |
| `install.sh` | Installs the tool, the Steam (Neutron) launcher and the login item |
| `patches/` | Patches per upstream project: `wine`, `proton`, `fex`, `dxmt`, `llvm-mingw` |
| `tool/` | The compat tool: `neutron` entry script, Steam manifests, Neutron.app Info.plist |
| `dev/` | `steam.sh` (Steam setup, game mapping), `steam-panel.js` (settings panel in Steam), `steamjs.mjs` (Steam JS calls over CEF) |
| `tests/` | Small Windows test programs: Steam bridge, D3D11 and D3D12 |
| `docs/` | Design, macOS findings, open points |

## Credits

neutron is glue and patches around the work of others:
[Wine](https://www.winehq.org),
[FEX](https://github.com/FEX-Emu/FEX),
[DXMT](https://github.com/3Shain/dxmt),
[Proton](https://github.com/ValveSoftware/Proton),
[llvm-mingw](https://github.com/mstorsjo/llvm-mingw),
[Wine Mono](https://github.com/madewokherd/wine-mono).
[Madeira](https://github.com/willfaust/Madeira) was the reference for running
this stack on Darwin.

## License

The repository holds only scripts and patches; each patch falls under the
license of the project it patches. Proton's lsteamclient is under the
Steamworks SDK license and is fetched at build time, never stored here.

Not affiliated with Valve or Apple. Steam is a trademark of Valve Corporation.
