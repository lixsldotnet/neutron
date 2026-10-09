# neutron reference

How neutron works and why it is built this way. User documentation is in the
[README](../README.md). Numbers in this document were measured on an M5 Max with
macOS 27 unless noted.

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

Everything except the game's own x86_64 code runs as arm64: Wine's system DLLs
are ARM64EC, so calls from the game into Windows APIs leave the emulator right
at the DLL boundary.

Steam runs `neutron waitforexitandrun <game exe>` in the game's folder.
`tool/neutron` sets up the prefix (`compatdata/<appid>/pfx`, FEX registered as
the x64 emulator, Steam registry keys, `steamclient64.dll`), applies the
settings and starts the game through Proton's `steam.exe`. Each game gets its
own prefix and its own app bundle (see Desktop integration).

## Steam integration

### Steam Play in the Mac client

Checked on client 1788652215.

- `steamclient.dylib` is universal and exports 5 of the 7 functions
  lsteamclient loads. `Steam_IsKnownInterface` and
  `Steam_NotifyMissingInterface` are optional in the proton patch.
- Steam Play is compiled in (`CCompatManager`, `compatibilitytools.d`,
  `CompatToolMapping`, all `STEAM_COMPAT_*` env vars). The settings page is
  hidden in the UI by a `"linux" == PLATFORM` check in the steamui JS.
- Proton's lsteamclient still has its `__APPLE__` path from 2018. The proton
  patch makes it build against Wine on macOS, finds `steamclient.dylib` in
  `Steam.AppBundle` (`NEUTRON_STEAMCLIENT_DYLIB`) and maps Windows virtual keys to
  macOS key codes.

### Turning it on

`install.sh` points Steam.app's `CFBundleExecutable` to a start script
(`Contents/MacOS/steam_neutron`). Valve's `steam_osx` is bound to the bundle's
`Info.plist`, so `steam_osx` and the bundle are signed ad hoc afterwards; Valve's
files are backed up and `install.sh --uninstall` restores them. The start script
and `dev/steam-hook.sh` do the following at every Steam start:

1. **Compat layer flag.** `CCompatManager` enables the compat layer once in its
   constructor when the ConVar `@sSteamCmdForcePlatformType` is `linux`. Steam
   ignores it on the command line but reads `steam_dev.cfg` from
   `Steam.AppBundle/Steam/Contents/MacOS/`. The start script writes that file
   at every start, so a Steam update that removes it does no harm.
2. **Tool discovery.** Steam scans `STEAM_EXTRA_COMPAT_TOOLS_PATHS` and
   `/usr/local/share/steam/compatibilitytools.d`, not the Steam root. The start
   script sets the first to `~/Library/Application Support/neutron/compatibilitytools.d`.
3. **CEF debug port.** Steam starts with `-cef-enable-debugging -devtools-port
   <port>` (8080 unless `install.sh --cef-port` is used; the start script exports
   it as `STEAM_CEF_PORT`). `dev/steamjs.mjs` evaluates JS in the
   SharedJSContext over this port.
4. **Back to macos.** Once the UI is up, the hook runs
   `SteamClient.Console.ExecCommand("@sSteamCmdForcePlatformType macos")`. The
   download platform is macOS again, the compat flag stays on.
5. **Manifest.** `to_oslist` in `toolmanifest.vdf` must be `linux`: tools are
   matched against the platform Steam started with (`macos` fails with
   eAppError 29).
6. **Remap after the switch.** After step 4 Steam shows "invalid platform"
   (display status 14) for every mapped game until its mapping changes; setting
   the same tool again does nothing. `dev/steam-sync.js` maps all games from
   config.vdf's `CompatToolMapping` to a second name of the same tool
   (`neutron_proton_remap`) and back, about 1 s for 200 games. The switch also
   marks installed games "Update Required"; the script queues an update, which
   is a 0-byte check that puts the Windows depot back.
7. **New games.** The same script maps every library game without a Mac version
   that is not in its seen list (`localStorage` key `neutron.seen` in the
   SharedJSContext), at start and every 30 s while Steam runs. Mapped games, Mac
   games and games set to none by the user are in the seen list and stay as
   they are.
8. **Compatibility tab.** `dev/steam-panel.js` adds a "Compatibility" tab to the
   game properties through `g_PopupManager` popup callbacks: the tool for the
   game (Neutron or none) and the neutron settings. If Steam shows its own
   Compatibility page, the settings go into that page instead.
9. **UI reloads.** Steam reloads its SharedJSContext now and then (new
   `CLIENT_SESSION`), which drops the tab and the new-game watch.
   `steam-hook.sh` keeps running while Steam runs and injects both again
   (without the remap). A hook from a newer Steam start takes over.

A per-game mapping (`SteamClient.Apps.SpecifyCompatTool`) is required. A global
tool (`SpecifyGlobalCompatTool`) alone does not unlock installing a Windows-only
game.

**Never clear a mapping on the way.** With no tool, Steam computes the Mac
depots, which a Windows-only game does not have, and deletes the installed files
("0 mounted depots", "341 deleted files" in `logs/content_log.txt`). When the
tool is back it downloads the whole game again, and a running download restarts
from 0. For the same reason `install.sh --uninstall` leaves the mappings alone.

### Settings in the launch options

Mac Steam does not support `VAR=x %command%` launch options (AppError 46). The
Compatibility tab writes `NEUTRON_<NAME>=<value>` words into the launch
options instead. Steam appends them to the tool's command line, and
`tool/neutron` exports them and removes them before the game starts. Settings
for all games go into `neutron.env` next to the tool. The idea of settings in
the launch options comes from
[NotProton](https://github.com/NotProtonNot/NotProton), which patches Steam's UI
in memory through an injected dylib; neutron injects JS over the CEF debug port
instead.

### Steam Cloud

The Mac client resolves the Windows roots of a compat tool game
(`WinAppDataLocalLow`, `WinMyDocuments` and co.) to
`compatdata/<appid>/pfx/drive_c/users/steamuser/...`, but only when the tool name
contains "proton" (`strstr(name, "proton")` in `steamclient.dylib`). Otherwise
every cloud file is skipped ("Steam Cloud out of sync"). So the internal tool
name is `neutron_proton` (shown as "Neutron"), the prefix is `pfx`, and the tool
runs Wine with `USER=steamuser`.

### Stop

Steam's Stop sends TERM to the tool. The tool runs the game in the background
and waits, so it handles TERM right away and ends the whole Wine session of the
prefix with `wineserver -k`.

## macOS findings

The problems a Windows-on-ARM64 stack hits on macOS, and how neutron solves
them.

- **x18.** Windows ARM64 keeps the TEB in x18, macOS clears x18 on syscalls and
  on preemption. `NtCurrentTeb()` reads the TEB from a pthread TSD slot via
  `TPIDRRO_EL0` instead (Wine `winnt.h`, the llvm-mingw `winnt.h`, FEX). The
  prebuilt llvm-mingw CRT startup objects are rebuilt with that header
  (`build.sh`). A fault handler covers the rest (libc++abi exception TLS).
- **`__PAGEZERO`.** arm64 macOS kills binaries whose `__PAGEZERO` ends below
  4 GB, so nothing can live there. `KUSER_SHARED_DATA` moves to
  `0x7ffffdfe0000`; the page after it holds the TSD slot offset and the PEB
  pointer for PE code (wine 0001, 0002, 0003).
- **execve** returns EFAULT for strings just below `0x7ffffe000000` (Wine's
  thread stacks), so exec env strings go to the heap.
- **JIT memory.** Write+exec needs `MAP_JIT`, which cannot be combined with
  `MAP_FIXED`, and the per-thread write/exec switch does not survive a signal
  handler. Wine maps FEX's code buffers with `MAP_JIT` at a hint address, FEX
  switches to write mode around code emission and patching (`JITWriteScope`).
  Other RWX requests get RW (wine 0003, fex 0001).
- **`mach_vm_map`** answers `KERN_INVALID_ADDRESS` above 4 GB near the main
  binary; Wine treats that as "in use" and keeps searching.
- **Free area search.** Wine finds free address space by trying `mach_vm_map`
  at each 64K step while holding `virtual_mutex`. Metal, system libraries and FEX
  map a lot that Wine does not track, so one allocation took thousands of
  syscalls and every thread that needed the lock stalled (Ready or Not: 1.5 to
  1.9 s freezes every few seconds, a 74 s black screen at start). On a collision
  Wine asks `mach_vm_region` for the colliding mapping and skips it in one step:
  first frame after 7.8 s, no periodic freezes (wine 0003).
- **16K pages.** Wine shows Windows 4K pages and gives all 4K pages of a host
  page the most permissive protection among them, so FEX's 4K guard pages never
  faulted: a call-return stack underflow (Unity/Mono) ran into the next mapping
  and every exception handler faulted again until the stack overflowed. FEX
  guard pages cover a whole 16K host page (fex 0002).
- **CPU features.** macOS traps EL0 reads of the ARM ID registers. Wine builds
  the `CP 40xx` registry values from `hw.optional.arm.FEAT_*`; without them FEX
  hides SSE4.2 and Unreal refuses to start (wine 0003).
- **Hardware TSO** (the x86 memory order Rosetta uses) cannot be enabled for a
  normal process, FEX emulates it with barriers (see `NEUTRON_FEX_TSO`).
- **Display lock deadlock.** The game thread held win32u's non-recursive display
  lock in `apply_display_settings` while `macdrv_set_display_mode` waited for the
  main thread and handled a `QUERY_MIN_MAX_INFO` event on the same thread, which
  called `GetSystemMetrics` and took the lock again. The lock is recursive for
  its owner (wine 0013). Abiotic Factor hung at startup in 3 of 3 runs before,
  in 0 of 7 after; Gamble With Your Friends started 20 of 20.
- **Never `cp` over a loaded Mach-O.** macOS kills the process with "Code
  Signature Invalid". Delete first, then copy.
- **SIP strips `DYLD_*`** when a system binary like `/usr/bin/perl` is in
  between, so Homebrew libraries are not found reliably. GnuTLS and FreeType are
  bundled into `files/lib`.

## Desktop integration

- **Game Mode** needs a real app bundle with the games category. Windows
  processes start from `files/lib/wine/aarch64-unix/Neutron.app` (a copy of the
  loader with `tool/Neutron-Info.plist`, wine 0006).
- **Per-game app bundle.** Each game gets its own bundle in its prefix folder
  (`compatdata/<appid>/Neutron.app`, loader hard-linked from the runtime, own
  bundle ID, the icon from the game's exe via `tool/peicon.py` and `sips`). Wine
  starts the game's processes from it (`NEUTRON_APP_BUNDLE`; the loader finds
  ntdll through `NEUTRON_NTDLL`), so the Dock, Cmd+Tab and window switchers show
  the game's name and icon. The menu bar shows the game name from Steam's app
  manifest (`NEUTRON_APP_NAME`, wine 0007).
- **Native fullscreen.** Borderless fullscreen windows go into macOS native
  fullscreen, their own Space with Game Mode (`NEUTRON_NATIVE_FULLSCREEN`,
  wine 0009).
- **Notch displays.** On a MacBook Pro, macOS puts native fullscreen windows
  below the notch and keeps the menu bar next to it, while Windows sees the whole
  screen; Unreal then fought over the window size (white screen). There games get
  a plain window over the whole screen and the app hides menu bar and Dock while
  active. External displays keep native fullscreen (wine 0007, 0009).
- **Cmd+Tab stays with macOS.** Games cannot register Alt+Tab as a hotkey
  (Command is Windows Alt), and windows in native fullscreen keep the normal
  window level, so the switcher and the Dock draw above them (wine 0008, 0009).
- **Activation.** macOS re-activates the app on every click (cause unknown).
  Wine answered each activation with a full display resync (registry rewrite,
  EDID reads, 100 to 170 ms on the game thread). It skips the resync when the
  displays did not change (wine 0007).
- **No display mode switches.** DXMT keeps the desktop mode when a game goes
  exclusive fullscreen at another resolution, and the Metal layer scales the
  backbuffer to the screen, like Proton's fullscreen hack. Real mode switches
  made the display flicker through several modes at start (dxmt 0009).
- **Retina mode** in Wine breaks Unreal (white window), so it stays off. On a
  HiDPI display games see points (a MacBook display is 1285x835 at render scale
  0.85).
- **Render scale.** `NEUTRON_RENDER_SCALE` (default 0.85) scales all Windows
  coordinates in winemac.drv the way Retina mode does with factor 2, so games see
  a 2924x1224 desktop on a 3440x1440 display. The tool sets the DXMT MetalFX
  factor to 1/scale (D3D11, and D3D12 through dxmt 0005), so the output is native
  size again. Only the desktop display mode is scaled (wine 0010).
- **Raw mouse.** Games read mouse look from raw input (`WM_INPUT`). Wine took it
  from the NSEvent deltas, which carry the macOS pointer acceleration and the
  render scale (mouse look 15% slower at 0.85). winemac.drv reads the device
  deltas from GameController `GCMouse` (`cocoa_rawmouse.m`, covers mice and the
  internal trackpad) and sends them as raw input only (`SEND_HWMSG_RAWINPUT`, as
  winewayland does); the cursor still follows the macOS cursor. GameController
  counts y up. `NEUTRON_RAW_MOUSE=0` turns it off, `WINEDEBUG=+rawmouse` logs the
  devices (wine 0014).
- **Direct presentation.** macOS shows a fullscreen game either "Direct" (no
  compositor pass) or "Composited"; the Metal HUD shows which in its top right.
  Two changes aim at Direct:
  10-bit RGB backbuffers (Unreal's default) get a BGR10A2 layer, the display's own
  order (dxmt 0010, `NEUTRON_LAYER_FORMAT=keep|bgra8` for comparison), and the
  drawable always has the view's size in screen pixels, which winemac.drv puts
  on the layer, with the present pass scaling the backbuffer into it (dxmt 0011,
  `NEUTRON_NATIVE_DRAWABLE=0` turns it off). With `WINEDEBUG=warn+macdrv`
  winemac.drv logs the windows visible when a window enters fullscreen and every
  window shown over it (Unreal helper windows on top force compositing).
  `dev/direct-test.sh` runs each case with the HUD.

## Game sandbox

`NEUTRON_SANDBOX=1` runs every Wine process of a prefix (wineboot, reg, steam.exe,
the game) under `sandbox-exec -f neutron.sb` with parameters for the home folder,
runtime, compatdata, game folder, Steam root, app id and the CEF port (`run_wine`
in tool/neutron). `sandbox-exec` is a SIP binary, so dyld drops `DYLD_*` for it;
the tool puts `DYLD_FALLBACK_LIBRARY_PATH` back through `/usr/bin/env`. The
profile allows by default and denies the dangerous parts explicitly; a deny-default
profile cannot be validated headless for AppKit windows, Game Mode and GCMouse. In
SBPL a more specific operation beats a wildcard regardless of order (`deny
file-read-data` wins over `allow file-read*`), so denies and allows use the same
level.

- wineserver opens files and sockets for its clients, so an unsandboxed server for
  the same prefix bypasses the sandbox (tested: reading `~/Documents` through it
  worked). The tool waits for an old server (`wineserver -w`) before `run`, and
  ntdll (patch 0003) refuses a server outside the sandbox when the client is in
  one (peer pid through `LOCAL_PEERPID`, then `sandbox_check`).
- Wine links the prefix's Documents, Desktop, Downloads, Music, Pictures and
  Videos to the real folders; with the sandbox they become real folders in the
  prefix.
- Steam's client library talks to Steam through POSIX shared memory (`/Shm/<hex>`),
  semaphores (`/Evt/<hex>`, `*.BinSemLock`), the Mach service
  `com.valvesoftware.steam.ipctool`, `registry.vdf` (Steam PID) and writes logs,
  `appcache/stats` and `userdata/<id>/<appid>` (Steam Cloud); the profile allows
  exactly these. The real Steam path still needs a test with a game.
- The CEF port is denied on 127.0.0.1 and ::1, other localhost ports stay open
  (`NEUTRON_SANDBOX_LOCALHOST=0` denies all localhost). securityd stays reachable:
  Wine's crypt32 loads the root certificates through it (HTTPS).
- Overhead: `sandbox-exec` adds about 5.5 ms per Wine start; `dev/bench/cpu.c`
  (server round trips, threads, memory) within noise.
- Steam's CEF port cannot be closed or switched at runtime (the Developer setting
  only takes effect at a Steam start, CEF's pipe mode cannot be passed through
  Steam). Chrome already rejects web pages (Origin and Host checks); the risk is
  local processes. A random port per start makes blind attacks on 8080 fail but
  not a targeted local scan.

## Performance work

Benchmarks and their conditions: [dev/bench/README.md](../dev/bench/README.md).
`NEUTRON_FPS_LOG=1` writes FPS and the slowest frame per second to the game
log, `NEUTRON_HUD=2` shows the Metal HUD with DXMT's per-frame statistics.

- **FEX TSO.** Fast mode is the default. Strict mode (`NEUTRON_FEX_TSO=strict`,
  vector and memcpy TSO) costs a lot in CPU-bound games (Gamble With Your
  Friends: 58 FPS strict, 98 FPS fast in the same view) and is set per game
  where it fixes races (`APP_TSO_427410=strict`: Abiotic Factor deadlocks between
  GameThread and SlateLoadingThread at startup without it).
- **FPCR on native calls.** An x64 to ARM64EC call cost 25 to 30 ns because FEX
  wrote FPCR twice (clear the AFP bits on exit, set NEP/AH on re-entry). EnterEC
  leaves AFP alone, ExitFunctionEC and SpillStaticRegs skip the write when the
  bits are already clear, and every IR block with vector register operands
  ensures NEP/AH at its start (mrs, tbnz, rarely orr+msr). Native calls from
  integer code cost about 5 ns, the d3d11 draw loop gained 19% FPS (fex 0003).
  `FEX_HOSTFEATURES=disableafp` is no alternative, it slows scalar float code.
- **x64 syscall stubs.** The ARM64EC x64 syscall stubs (`__ASM_SYSCALL_FUNC`)
  tested `0x7ffe0308`, which faults since KUSER lives above 4 GB, on every
  direct Nt call (2.4 us per call, mostly signal handling). The test is
  `cmp %eax,%eax` plus a nop of the same size (the jne is never taken), and
  `arm64x_check_call` in `signal_arm64ec.c` accepts both forms. Direct x64 `Nt*`
  calls: 0.17 us (wine 0002, 0003).
- **Wineserver QoS and timers.** The wineserver thread runs at
  `QOS_CLASS_USER_INTERACTIVE`, normal threads use latency tier 0 (tier 1
  doubles timer leeway), and a non-alertable `NtDelayExecution` waits on a
  one-shot critical kqueue timer instead of `select()`. `Sleep(1)` takes 1.04 ms
  instead of 1.49 ms; server round trips do not change.
  `NEUTRON_PRECISE_TIMERS=0` switches the timer part off (wine 0012).
- **Lazy DXGI output description.** `MTLDXGIOutputImpl` queried the ColorSync
  profile (an XPC round trip, about 0.45 ms) in its constructor, and Gamble
  creates outputs every frame through `GetContainingOutput`. The query runs once,
  in `GetDesc1`, and the unix side releases the ColorSync profiles and tag data it
  leaked. `EnumOutputs`: 0.455 ms to 0.0003 ms. Gamble uncapped (`gwyfs`): 480 to
  575 FPS, CPU per frame 4.43 to 3.95 ms (dxmt 0008).
- **LLVM without assertions.** The LLVM 15 behind DXMT's airconv is built with
  `LLVM_ENABLE_ASSERTIONS=Off`. Converting 68 DXBC shaders: 0.525 s to 0.472 s
  (-10%), winemetal.so 27 MB to 22 MB, same output.
- **DXMT caches in compatdata.** The shader cache (`shaders_320.db`) and the
  Metal PSO cache (`com.apple.metal`) live in `compatdata/<appid>/dxmt-cache`
  (the tool sets `DXMT_SHADER_CACHE_PATH`, dxmt 0006 uses it for the Metal cache
  too) instead of the macOS user cache dir, which the system can purge. This is
  for durability; no speed change was measured.
- **HUD cost.** `NEUTRON_HUD=2` loads libMetalMetricsInterpose and costs about
  11% game CPU per frame in the Gamble menu. Benchmarks run without it.

## D3D12 and DXIL

Unreal 5 decides D3D12 support from the device's caps: feature level 12_0, shader
model 6.x, resource binding tier 3, wave ops and 64-bit atomics (Nanite), and asks
OPTIONS9 (feature 37), which upstream DXMT did not answer. DXMT's D3D12 only
compiled DXBC (SM 5.1), so Ready or Not with `-dx12` showed "DirectX 12 is not
supported on your system".

- DXMT patch 0012 answers all remaining feature queries (unsupported features as
  "not supported" with S_OK) and exports `D3D12EnableExperimentalFeatures`; 0013
  makes `D3D12CreateDevice` fail for a minimum feature level above what the device
  reports, like a real GPU.
- 0014 is a DXIL front end in airconv: LLVM 15 reads the DXIL bitcode, `dx.op`
  calls (about 120 opcodes) become AIR, resources and root signatures reuse the
  DXBC SM 5.1 path, signatures go through the DXBC signature handlers. VS, PS and
  CS; SM 6.6 `ResourceDescriptorHeap`/`SamplerDescriptorHeap`; wave and quad ops
  on Metal SIMD groups (32 lanes); compute derivatives through quad shuffles;
  64-bit atomics only as unsigned InterlockedMin/Max on R32G32_UINT textures and
  buffers without return value (Nanite), in AIR 2.9 (Metal's compiler crashes on
  them in 2.7). PSO failures log the missing operation by name.
- 0015: `d3d12.shaderModel = 66` (DXMT config, `NEUTRON_D3D12_SM6=1` in the tool)
  reports FL 12_0, SM 6.6, binding tier 3, wave ops, Int64 and Atomic64. Default
  stays SM 5.1.
- Tests (headless): `tests/d3d12_caps.c` reproduces Unreal 5's adapter checks,
  `tests/d3d12_dxil_*.c` cover compute, draw, MRT, ops, descriptor heap indexing,
  64-bit atomics and PSO creation. 204 of 243 vkd3d-proton SM6 test shaders convert.
- Missing: geometry, hull and domain shaders in DXMT's D3D12 (DXBC too), 16-bit
  shader types, doubles, other 64-bit atomics, SM 6.7+, mesh shaders, ray tracing;
  `ResolveQueryData` is a no-op; no D3D12 shader cache (DXIL is converted for every
  PSO). Not yet run with a real Unreal 5 game.

## msync

Every Windows sync call (SetEvent, WaitForSingleObject, ReleaseSemaphore) was a
round trip to wineserver: 16.6 us for an event ping-pong between two threads,
6.7 us for a wait on a signaled event, 5.7 us for SetEvent (M5 Max). Unreal's
render thread spends its time there. Wine 11 routes every waitable object through
its `inproc_sync` layer (ntsync on Linux); patch 0015 adds a macOS backend for it,
a user-space copy of the ntsync driver:

- wineserver creates a 64 MB `shm_open` region (pages are only backed when
  touched) and sends its fd as the inproc device. Objects are 64-byte slots, the
  slot index replaces the ntsync object fd in the `get_inproc_sync_fd` and
  `get_inproc_alert_fd` replies.
- Each thread has a wait block; a wait links one entry per object into the
  object's waiter list, a signaler satisfies waiters under the object lock and
  wakes them with `os_sync_wake_by_address_any` after unlocking, waiters spin
  2 us (`NEUTRON_MSYNC_SPIN`) before `os_sync_wait_on_address`. Wait-all takes
  one global lock like ntsync.
- wineserver still owns lifetime, names, handles and access checks, signals the
  internal syncs (process and thread exit, APC alert) and abandons mutexes through
  the same code. Lock sections are protected against Wine's suspend and terminate
  signals by a per-thread busy flag (the handlers re-raise when the section ends).
- Measured (median of 5): event and semaphore ping-pong 0.55 us (was 18 us), wait
  on a signaled event 27 ns (7.7 us), SetEvent 24 ns (6.9 us). `tests/sync_semantics.c`
  (56 checks: wait any/all, alertable waits and APCs, abandoned mutexes, PulseEvent,
  SignalObjectAndWait, cross-process and named objects, killed waiters, stress)
  passes with and without msync.
- Limits: a process killed with SIGKILL inside a lock section blocks the users of
  that object; slots of objects still open at process exit stay until wineserver
  ends; 512K objects, 32K threads. Off unless `NEUTRON_MSYNC=1` until tested with
  more games.

## Patches

All third-party code is fetched at build time at a pinned version and patched
by `build.sh`. Wine and llvm-mingw patches are applied with `patch -p1`, FEX and
DXMT patches with `git apply`, all in file name order.

| Upstream | Version |
|---|---|
| Wine | 11.19 |
| Proton (lsteamclient, steam_helper) | `proton_11.0`, 5b89db9 |
| FEX | FEX-2610, 14c9268 |
| DXMT | e94c312 (2026-10-06) |
| LLVM (for DXMT airconv) | 15.0.7 |
| llvm-mingw | 20260908, CRT startup objects rebuilt from mingw-w64 9f55f4a |
| Wine Mono | 11.3.0 |

| Patch | What |
|---|---|
| `wine/0001-configure-native-macos` | configure: loader link flags for arm64 macOS (`__PAGEZERO` above 4 GB, 16K segments) |
| `wine/0002-kuser-shared-data-above-4g` | `KUSER_SHARED_DATA` at `0x7ffffdfe0000`, x64 syscall stub test |
| `wine/0003-ntdll-darwin-arm64` | ntdll: TEB via TSD, `MAP_JIT`, exec env strings, free area search, CPU feature registry values |
| `wine/0004-winemac-dxmt-metal-view` | Metal view helpers for DXMT's winemetal.so |
| `wine/0005-makedep-version-res-arm64ec` | makedep: version resources for arm64ec-only modules |
| `wine/0006-loader-game-mode` | Loader in an app bundle (games category, per-game bundle) |
| `wine/0007-winemac-app-name` | Game name in menu bar and Dock, notch handling, no display resync on activation |
| `wine/0008-winemac-keep-cmd-tab` | Cmd+Tab stays with macOS |
| `wine/0009-winemac-native-fullscreen` | Borderless fullscreen windows in macOS native fullscreen |
| `wine/0010-winemac-render-scale` | `NEUTRON_RENDER_SCALE` |
| `wine/0011-appwiz-no-addon-dialog` | No Mono/Gecko download dialog |
| `wine/0012-server-qos-precise-timers` | Wineserver QoS, latency tier, kqueue sleep timers |
| `wine/0013-win32u-recursive-display-lock` | Display lock recursive for its owner |
| `wine/0014-winemac-raw-mouse` | Raw mouse input from GameController `GCMouse` |
| `proton/0001-lsteamclient-macos` | lsteamclient on macOS: optional exports, libc++, key map, `NEUTRON_STEAMCLIENT_DYLIB` |
| `proton/0002-lsteamclient-link-libcxx` | Links lsteamclient.so against libc++ |
| `proton/0003-steam-helper-macos` | steam.exe opens `steam://` URLs with `open` |
| `fex/0001-macos-teb-tsd-and-map-jit` | TEB via TSD, `MAP_JIT` write scopes, macOS UnixLib |
| `fex/0002-host-page-guards` | Guard pages cover a whole host page |
| `fex/0003-afp-lazy-native-transition` | Fewer FPCR writes on x64 to ARM64EC calls |
| `llvm-mingw/0001-ntcurrentteb-tsd` | `NtCurrentTeb()` via TSD in `winnt.h` |
| `dxmt/0001-d3d12-clock-calibration` | D3D12 timestamp frequency and clock calibration |
| `dxmt/0002-no-thread-local` | No `thread_local` in the occlusion query code |
| `dxmt/0003-neutron-hud-line` | neutron lines in the Metal HUD, statistics with `NEUTRON_HUD=2` |
| `dxmt/0004-neutron-fps-log` | `NEUTRON_FPS_LOG` |
| `dxmt/0005-d3d12-metalfx-spatial` | MetalFX spatial upscaling for D3D12 swapchains |
| `dxmt/0006-cache-in-compatdata` | Metal cache in `DXMT_SHADER_CACHE_PATH` |
| `dxmt/0007-bench-novsync` | `NEUTRON_BENCH_NOVSYNC` (benchmarks only) |
| `dxmt/0008-lazy-display-desc-cfrelease` | ColorSync query once per output, leaks fixed |
| `dxmt/0009-no-display-mode-switch` | No real display mode switches |
| `dxmt/0010-display-layer-format` | BGR10A2 layer for 10-bit backbuffers, `NEUTRON_LAYER_FORMAT` |
| `dxmt/0011-native-drawable-size` | Drawable in screen pixels, `NEUTRON_NATIVE_DRAWABLE` |

## Open points

- Abiotic Factor shows a black screen while loading a map: `LoadMap` stalls up to
  60 s while the GPU runs compute work.
- 32-bit games do not run and are out of scope: their image must load below
  4 GB, which arm64 macOS does not allow (BO2 `t6mp.exe`: `map_free_area` in
  0x10000-0x7fff0001 fails with c0000017). Only a Rosetta x86_64 Wine could run
  them.
- Allocations below 4 GB fail (seen once, the image relocated fine).
- The Steam overlay and anti-cheat (EAC, BattlEye) do not work.
- Direct presentation: the changes in dxmt 0010 and 0011 still need to be
  verified on screen with `dev/direct-test.sh`.
- D3D12 support is being worked on; a section on it will follow.
