# neutron: design and findings

Goal: the native Mac Steam client runs Windows games like Proton does on Linux.
No second Windows Steam inside Wine, no Rosetta. Apple Silicon only.

## Architecture

```
Mac Steam (native, the only Steam)
  | starts Windows games through the compat tool "neutron_proton"
  v
tool/neutron                          prefix setup, env, per-game settings
  v
Wine 11.19, arm64 unix side, ARM64EC + aarch64 Windows side
  +- FEX (libarm64ecfex.dll)          translates only the game's x86_64 code
  +- DXMT (d3d10/11/12, dxgi)         Direct3D on Metal, MetalFX upscaling
  +- steam.exe (Proton steam_helper)  Steam registry and process state
  +- steamclient64.dll = lsteamclient Windows side of the Steam bridge
       v  Wine unix call
     lsteamclient.so                  macOS side of the bridge
       v  dlopen
     steamclient.dylib (Mac Steam)    login, ownership, achievements, cloud
```

Everything except the game's own x86_64 code runs as arm64: Wine's system
DLLs are ARM64EC, so calls from the game into Windows APIs leave the emulator
right at the DLL boundary.

## Steam Play in the Mac client

Checked on client 1788652215.

- `steamclient.dylib` is universal and exports 5 of the 7 functions
  lsteamclient loads (`Steam_IsKnownInterface` and
  `Steam_NotifyMissingInterface` are optional in our patch).
- Steam Play is compiled in (`CCompatManager`, `compatibilitytools.d`,
  `CompatToolMapping`, all `STEAM_COMPAT_*` env vars). The settings page is only
  hidden in the UI by a `"linux" == PLATFORM` check in the steamui JS.
- Proton's lsteamclient still has its `__APPLE__` path from 2018.

How it is turned on (Steam.app start script from `install.sh`, then
`dev/steam-hook.sh`):

1. **Compat layer flag.** `CCompatManager` enables the compat layer once in its
   constructor when the ConVar `@sSteamCmdForcePlatformType` is `linux`. Steam
   ignores it on the command line but reads `steam_dev.cfg` from
   `Steam.AppBundle/Steam/Contents/MacOS/`.
2. **Back to macos.** After startup `SteamClient.Console.ExecCommand(
   "@sSteamCmdForcePlatformType macos")` sets the download platform back; the
   compat flag stays on.
3. **Tool discovery.** Steam scans `STEAM_EXTRA_COMPAT_TOOLS_PATHS` and
   `/usr/local/share/steam/compatibilitytools.d`, not the Steam root.
4. **Manifest.** `to_oslist` must be `linux`, tools are matched against the
   platform Steam started with (`macos` fails with eAppError 29).
5. **Mapping.** `SteamClient.Apps.SpecifyCompatTool(appid, "neutron_proton")` over the
   CEF debug port (`-cef-enable-debugging -devtools-port 8080`). A global tool
   (`SpecifyGlobalCompatTool`) alone does not unlock installing a Windows-only
   game, the per-game mapping does.
6. **Remap after the switch.** After step 2 Steam shows "invalid platform"
   (display status 14) for every mapped game until its mapping changes; setting
   the same tool again does nothing. `dev/steam-sync.js` maps all games from
   config.vdf's `CompatToolMapping` to a second name of the same tool
   (`neutron_proton_remap`) and back, about 1 s for 200 games. Never clear the
   mapping on the way: with no tool, Steam computes the Mac depots, which a
   Windows-only game does not have, deletes the installed files ("0 mounted
   depots", "341 deleted files" in `logs/content_log.txt`), and downloads the
   whole game again once the tool is back. A running download restarts from 0.
7. **New games.** The same script maps every library game without a Mac version
   that is not in its seen list (`localStorage` key `neutron.seen` in the
   SharedJSContext) at start and every 30 s while Steam runs. Games mapped, Mac
   native, or set to none by the user are in the seen list and stay as they are.

Steam Cloud works like with Proton: the Mac client resolves the Windows roots
of a compat tool game (`WinAppDataLocalLow`, `WinMyDocuments` and co.) to
`compatdata/<appid>/pfx/drive_c/users/steamuser/...`, but only when the tool
name contains "proton" (`strstr(name, "proton")` in `steamclient.dylib`).
Otherwise every cloud file is skipped ("Steam Cloud out of sync"). So the
internal tool name is `neutron_proton` (shown as "Neutron"), the prefix is
`pfx`, and the tool runs Wine with `USER=steamuser`; older prefixes are
migrated on start.

Per-game settings: Mac Steam hides the Compatibility page (the same `"linux"`
platform check), so `dev/steam.sh` adds its own "Compatibility" tab to a game's
properties at runtime (`dev/steam-panel.js`, through `g_PopupManager` popup
callbacks in the SharedJSContext): the tool for the game (Neutron or none) and the
neutron settings. It writes `NEUTRON_<NAME>=<value>` words
into the launch options, which Steam appends to the tool's command line, and
`tool/neutron` takes them out again. The idea of settings in the launch options
comes from NotProton (github.com/NotProtonNot/NotProton), which patches Steam's
UI in memory through an injected dylib; neutron changes no Steam file.

Steam then downloads the Windows depots itself and runs
`neutron waitforexitandrun <game exe>`. Mac Steam does not support
`VAR=x %command%` launch options (AppError 46), so settings go into
`neutron.env` next to the tool.

## macOS findings

These are the problems a Windows-on-ARM64 stack hits on macOS and how neutron
solves them. Measured on macOS 27, M5 Max.

- **x18.** Windows ARM64 keeps the TEB in x18, macOS clears x18 on syscalls
  and on preemption. `NtCurrentTeb()` reads the TEB from a pthread TSD slot via
  `TPIDRRO_EL0` instead (Wine `winnt.h`, the llvm-mingw `winnt.h`, FEX). The
  prebuilt llvm-mingw CRT startup objects are rebuilt with that header. A fault
  handler safety net covers the rest (libc++abi exception TLS).
- **`__PAGEZERO`.** arm64 macOS kills binaries whose `__PAGEZERO` ends below
  4 GB, so nothing lives there. `KUSER_SHARED_DATA` moves to `0x7ffffdfe0000`;
  the page after it holds the TSD slot offset and the PEB pointer for PE code.
- **execve** returns EFAULT for strings just below `0x7ffffe000000` (Wine's
  thread stacks), exec env strings go to the heap.
- **JIT memory.** Write+exec needs `MAP_JIT`, which cannot be combined with
  `MAP_FIXED`, and the per-thread write/exec switch does not survive a signal
  handler. Wine maps FEX's code buffers with `MAP_JIT` at a hint address, FEX
  switches to write mode around code emission and patching (`JITWriteScope`).
  Other RWX requests get RW.
- **`mach_vm_map`** answers `KERN_INVALID_ADDRESS` above 4 GB near the main
  binary; Wine treats that as "in use" and keeps searching.
- **16K pages.** Wine shows Windows 4K pages and gives all 4K pages of a host
  page the most permissive protection of them. FEX's 4K guard pages never
  faulted: a call-return stack underflow (Unity/Mono) ran into the next
  mapping and every exception handler faulted again until the stack overflowed.
  FEX guard pages are 16K now.
- **CPU features.** macOS traps EL0 reads of the ARM ID registers. Wine builds
  the `CP 40xx` registry values from `hw.optional.arm.FEAT_*`, otherwise FEX
  hides SSE4.2 and Unreal refuses to start.
- **Hardware TSO** (the x86 memory order Rosetta uses) cannot be enabled for a
  normal process, FEX emulates it with barriers.
- **Never `cp` over a loaded Mach-O**, macOS kills the process with "Code
  Signature Invalid". Delete first, then copy.
- **SIP strips `DYLD_*`** when a system binary like `/usr/bin/perl` is in
  between. GnuTLS and FreeType are bundled into `files/lib`.

## Desktop integration

- **Game Mode** needs a real app bundle with the games category. Windows
  processes start from `files/lib/wine/aarch64-unix/Neutron.app` (a copy of the
  loader with `tool/Neutron-Info.plist`), borderless fullscreen windows go into
  macOS native fullscreen (`NEUTRON_NATIVE_FULLSCREEN`).
- The menu bar shows the game name from Steam's app manifest
  (`NEUTRON_APP_NAME`), not "Wine".
- Each game gets its own app bundle in its prefix folder
  (`compatdata/<appid>/Neutron.app`, loader hard-linked from the runtime, own
  bundle ID, the icon from the game's exe via `tool/peicon.py` and `sips`). Wine
  starts the game's processes from it (`NEUTRON_APP_BUNDLE`, the loader finds
  ntdll through `NEUTRON_NTDLL`), so the Dock, Cmd+Tab and window switchers show
  the game's name and icon.
- Cmd+Tab stays with macOS: games cannot register Alt+Tab as a hotkey (Command
  is Windows Alt), and windows in native fullscreen keep the normal window level
  so the switcher and the Dock draw above them.
- Every click re-activates the app (cause unknown). Wine used to answer each
  activation with a full display resync (registry rewrite, EDID reads, 100 to
  170 ms on the game thread); it now skips the resync when the displays did not
  change.
- No real display mode switches: DXMT keeps the desktop mode when a game goes
  exclusive fullscreen at another resolution and the Metal layer scales the
  backbuffer to the screen (like Proton's fullscreen hack). Before, games made
  the MacBook display flicker through several modes at start.
- Notch displays (MacBook Pro): macOS puts native fullscreen windows below the
  notch and keeps the menu bar next to it, while Windows sees the whole screen;
  Unreal then fought over the window size (white screen). There games get a plain
  window over the whole screen and the app hides menu bar and Dock while active.
  External displays keep native fullscreen.
- Wine's Retina mode breaks Unreal (white window), so it stays off: on a HiDPI
  display games see points (MacBook 1285x835 at render scale 0.85). Open point.
- Steam's Stop: the tool runs the game in the background and ends the whole
  Wine session of the prefix with `wineserver -k` on TERM.

## Performance

- **Startup hang (display lock).** Abiotic Factor hung at startup after 2-3 FPS lines
  (found with `dev/bench/hang-watch.sh`, which samples and lldb-dumps a stalled
  game). GameThread held win32u's non-recursive display lock in
  `apply_display_settings` while `macdrv_set_display_mode` waited for the main
  thread and handled a QUERY_MIN_MAX_INFO on the same thread, which called
  GetSystemMetrics and locked again (self-deadlock). Patch 0013 makes the lock
  recursive for its owner. AF reaches the game 7 of 7 (was 0 of 3), Gamble 20 of 20.
- **FEX TSO.** Fast mode is the default. Strict mode (`NEUTRON_FEX_TSO=strict`,
  vector and memcpy TSO) costs a lot in CPU-bound games (Gamble With Your
  Friends: 58 FPS strict, 98 FPS fast in the same view) and is set per game
  where it fixes races (`APP_TSO_427410=strict`: Abiotic Factor deadlocks
  between GameThread and SlateLoadingThread at startup without it).
- **FPCR on native calls.** An x64 to ARM64EC call used to cost 25-30 ns because
  FEX wrote FPCR twice (clear AFP bits on exit, set NEP/AH on re-entry). Now
  EnterEC leaves AFP alone, ExitFunctionEC and SpillStaticRegs skip the write
  when the bits are already clear, and every IR block with vector register
  operands ensures NEP/AH at its start (mrs, tbnz, rare orr+msr). Native calls
  from integer code cost about 5 ns, d3d11 draw loop +19% FPS (patch
  `0003-afp-lazy-native-transition`). Do not use `FEX_HOSTFEATURES=disableafp`,
  it slows scalar float code.
- **Wineserver QoS and timers.** The wineserver thread runs at
  QOS_CLASS_USER_INTERACTIVE, normal threads use latency tier 0 (tier 1 doubles
  timer leeway), and a non-alertable NtDelayExecution waits on a one-shot
  critical kqueue timer instead of select(). Sleep(1) went from 1.49 ms to
  1.04 ms; server round trips did not change. `NEUTRON_PRECISE_TIMERS=0`
  switches the timer part off (patch `0012-server-qos-precise-timers`).
- **Uncapped Gamble profile (bench only).** The Gamble menu sits at the 160 Hz
  display cap, also with strict TSO (4.9 ms CPU per frame, still 160 FPS), so FPS
  hid every gain. `NEUTRON_BENCH_NOVSYNC=1` (patch `0007-bench-novsync`, never set
  by the tool) makes DXMT draw the present pass into an offscreen texture and skip
  `nextDrawable`/present, because `Present(0)` and `displaySyncEnabled = NO` do not
  lift the compositor's drawable pacing. `dev/bench/run.sh` profile `gwyfs`
  (strict TSO plus this switch) gives `gwyfs.fps` (about 464, spread 6%),
  `wall_ms_per_frame` (2.16) and `cpu_per_frame_ms` (4.58). Limits: the menu is
  light, strict only costs about 3% there (464 against 476 FPS fast), so a TSO
  change is hardly visible; the in-game view is not driveable. New metric
  `wall_ms_per_frame` is also written for the other game profiles.
- **Lazy DXGI output description.** `MTLDXGIOutputImpl` queried the ColorSync profile
  (XPC round trip, about 0.45 ms) in its constructor, and Gamble creates outputs every
  frame through `GetContainingOutput`. The query now runs once, in `GetDesc1` (patch
  `0008-lazy-display-desc-cfrelease`), and the unix side releases the ColorSync profiles
  and tag data it leaked. `EnumOutputs` 0.455 ms to 0.0003 ms; `gwyfs.fps` 480 to 575
  (+20%), CPU per frame 4.43 to 3.95 ms.
- **x64 syscall stub.** The ARM64EC x64 syscall stubs (`__ASM_SYSCALL_FUNC`) tested
  `0x7ffe0308`, which faulted on every direct Nt call since KUSER moved above 4 GB
  (2.4 us per call, mostly signal handling). The test is now `cmp %eax,%eax` plus a
  nop of the same size (jne never taken), and `arm64x_check_call` in
  `signal_arm64ec.c` accepts both forms. Direct x64 `Nt*` calls: 2.4 us to 0.17 us.
- **LLVM without assertions.** The LLVM 15 behind DXMT airconv is built with
  `LLVM_ENABLE_ASSERTIONS=Off`. Converting 68 DXBC shaders: 0.525 s to 0.472 s (-10%),
  winemetal.so 27 MB to 22 MB, output unchanged. No visible effect on game start.
- **DXMT caches in compatdata.** The shader cache (`shaders_320.db`) and the Metal PSO
  cache (`com.apple.metal`) live in `compatdata/<appid>/dxmt-cache` (tool sets
  `DXMT_SHADER_CACHE_PATH`, patch 0006 makes dxgi use it for the Metal cache too)
  instead of the macOS user cache dir, which the system can purge. The old caches are
  copied once, never deleted. Durability only, no speed change measured.
- **Render scale.** `NEUTRON_RENDER_SCALE` (default 0.85) scales all Windows
  coordinates in winemac.drv the way Retina mode does with factor 2, so games
  see a 2924x1224 desktop on a 3440x1440 display. The tool sets the DXMT MetalFX
  factor to 1/scale (D3D11 and D3D12), and the output is native size again.
  Only the desktop display mode is scaled; real mode switches are not.
- **Raw mouse.** Wine took the game's raw input (`WM_INPUT`, mouse look in Unity
  and Unreal) from the NSEvent deltas, which carry the macOS pointer acceleration
  and the render scale (mouse look 15% slower at 0.85). Patch 0014 reads the device
  deltas from GameController `GCMouse` (`cocoa_rawmouse.m`, covers mice and the
  internal trackpad) and sends them as raw input only (`SEND_HWMSG_RAWINPUT`, as
  winewayland does); the cursor still follows the macOS cursor. GameController
  counts y up. `NEUTRON_RAW_MOUSE=0` turns it off, `WINEDEBUG=+rawmouse` logs the
  devices.
- **Measuring.** `NEUTRON_FPS_LOG=1` writes FPS and the slowest frame per
  second to the game log, `NEUTRON_HUD=2` shows the Metal HUD with DXMT's
  per-frame statistics.
- **HUD off for measuring.** `NEUTRON_HUD=2` (Metal HUD plus DXMT per-frame
  statistics, loads libMetalMetricsInterpose) costs about 11% game CPU per frame in
  the Gamble menu (4.56 ms without, 5.09 ms with, 3 runs each, FPS stays at the
  display cap). `dev/bench/run.sh` unsets `NEUTRON_HUD`/`MTL_HUD_*` and records
  them as `meta.hud_unset`; `BENCH_KEEP_HUD=1` keeps them. No runtime change.

## Patches

All third-party code is fetched at build time and patched (`build.sh`).

| Folder | Upstream | What |
|---|---|---|
| `patches/wine` | Wine 11.19 | configure for arm64 macOS, KUSER above 4 GB, ntdll TEB/JIT/exec/ID registers, winemac for DXMT, makedep, Game Mode loader, app name, Cmd+Tab, native fullscreen, render scale |
| `patches/proton` | Proton lsteamclient, steam_helper | lsteamclient on macOS (optional exports, libc++, key map, `NEUTRON_STEAMCLIENT_DYLIB`), libc++ link, `steam://` URLs via `open` |
| `patches/llvm-mingw` | llvm-mingw 20260908 | `NtCurrentTeb()` via TSD in `winnt.h` |
| `patches/fex` | FEX 2610 | TEB via TSD, `MAP_JIT` write scopes, macOS UnixLib, 16K guard pages |
| `patches/dxmt` | DXMT main | D3D12 clock calibration, no `thread_local`, neutron HUD lines, FPS log, D3D12 MetalFX |

## Open points

- Black loading screen in Abiotic Factor: `LoadMap` stalls up to 60 s while the
  GPU runs compute work.
- 32-bit games do not run and are out of scope: their image must load below 4 GB,
  which arm64 macOS does not allow (BO2 `t6mp.exe`: `map_free_area` in
  0x10000-0x7fff0001 fails, c0000017). Only a Rosetta x86_64 Wine could run them.
- Allocations below 4 GB fail (seen once, relocated fine).
- Overlay and anti-cheat (EAC, BattlEye) will not work.
- Whether a Steam client update removes `steam_dev.cfg`.
- License: the lsteamclient folder is under the Steamworks SDK license, not
  open source. The repo only holds patches and fetches Valve's code at build
  time; check this before publishing binaries.
