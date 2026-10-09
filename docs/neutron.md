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
   is a 0-byte check that puts the Windows depot back. At the same moment Steam
   starts its automatic update of recently played games while their mapping is
   still invalid: it finds no Mac depots ("1 active, 0 target"), deletes the
   installed files and downloads them all again after the remap (seen for a game
   played 20 minutes before every Steam start). So the script sets installed
   neutron games to "only update when launched" (`SetAppAutoUpdateBehavior(id, 1)`,
   also for games installed later, by the 30 s watch); the queued check above is a
   user-started update and still brings real game updates at every Steam start.
   Pausing all downloads (`EnableAllDownloads(false)`) over the switch does not stop
   it, and holding the updates in a step before the switch delayed the switch so
   much that the remap ran in the wrong platform state and cleared the mappings.
7. **New games.** The same script maps every library game without a Mac version
   that is not in its seen list (`localStorage` key `neutron.seen` in the
   SharedJSContext), at start and every 30 s while Steam runs. Mapped games, Mac
   games and games set to none by the user are in the seen list and stay as
   they are.
8. **Compatibility tab.** `dev/steam-panel.js` adds a "Compatibility" tab to the
   game properties through `g_PopupManager` popup callbacks: the tool for the
   game (Neutron or none) and the neutron settings. If Steam shows its own
   Compatibility page, the settings go into that page instead.
   The same script marks the library list: right of each game name a 16 px
   badge, the neutron icon for games mapped to neutron and the Apple logo (system
   font glyph) for native Mac games. The list is virtualized, so a tick once a
   second plus a scroll listener fix the visible rows; the kinds of the whole
   library are loaded in the background at start. Steam's library CSS paints every
   SVG circle and path semi-transparent white, so the icon colors are inline
   `!important` styles.
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
  Blocks without float math, with compares or with a few add/sub/mul/div before a
  native call skip the FPCR write too: about 5 ns after float code (fex 0004, see
  D3D11 CPU cost).
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
- 0029: geometry, hull and domain shaders, emulated like DXMT's D3D11 does: a Metal
  mesh pipeline with the VS (or VS+HS) as object function and the GS (or DS) as mesh
  function. The object function depends on the draw (index format, strip topology), so
  a PSO builds these variants on first use (the non-indexed list one at creation, so a
  shader the emulation cannot handle fails `CreateGraphicsPipelineState`). A GS that
  only passes through the render target array or viewport index is folded into the VS.
  Root parameters with GEOMETRY, HULL or DOMAIN visibility go to the object and mesh
  stages.
- 0030: the same for DXIL: GS emit, cut, `SV_GSInstanceID` and `SV_PrimitiveID`; HS with
  the control point function on the threads of a patch and the patch constant function
  on its first thread after a barrier; DS domain location and patch constants.
- 0028: hull shaders from vkd3d-shader (Wine's `D3DCompile`, which games that compile
  HLSL at run time get) index control points with `vOutputControlPointID` itself; the
  converter crashed on them, D3D11 included.
- 0031: queries, bundles and the command list methods that aborted. Occlusion and
  binary occlusion count into the query heap's buffer, used as visibility result buffer
  of the render pass (slot 0 stays unused: every such pass also writes offset 0).
  Timestamps: a blit pass samples the GPU clock (nanoseconds, matches
  `GetTimestampFrequency`) into a counter sample buffer; Metal fills the samples only
  once their command buffer has completed, so a timestamp `ResolveQueryData` waits on
  an event the queue signals after that completion (a GPU bubble when it is in the same
  frame). Pipeline and stream output statistics resolve to zeros. `ExecuteBundle`
  replays the bundle's calls on the calling list. Buffer to buffer `CopyTextureRegion`,
  `WriteBufferImmediate`, `ResolveSubresourceRegion` for whole regions, and
  `DiscardResource` (DontCare store of the pass that just wrote the texture and load in
  the pass right after, like D3D11's 0016).
- Tests (headless): `tests/d3d12_caps.c` reproduces Unreal 5's adapter checks,
  `tests/d3d12_dxil_*.c` cover compute, draw, MRT, ops, descriptor heap indexing,
  64-bit atomics, PSO creation, geometry and tessellation shaders; `tests/d3d12_gs.c`,
  `tests/d3d12_tess.c` (DXBC), `tests/d3d12_queries.c` and `tests/d3d12_cmdlist.c`.
  204 of 243 vkd3d-proton SM6 test shaders convert.
- Missing: stream output, a GS after tessellation (unless pass-through), tessellator
  point and line output, isolines, `ExecuteIndirect` with GS or tessellation pipelines
  (skipped with a warning), 16-bit shader types, doubles, other 64-bit atomics, SM 6.7+,
  mesh shaders, ray tracing; no D3D12 shader cache (DXIL is converted for every PSO).
  An occlusion query that spans render passes keeps only the count of the last one,
  and queries of two heaps in one render pass count only the first heap. Partial
  `ResolveSubresourceRegion` is skipped. Not yet run with a real Unreal 5 game.

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

## D3D11 CPU cost

`dev/bench/d3d11_headless.c` renders offscreen like an engine (per draw a
`Map(WRITE_DISCARD)` of a constant buffer, shader, texture, state switches;
variants for `UpdateSubresource`, many bindings, instancing, per-draw state,
deferred contexts) and prints app-thread submit time, process CPU per frame and an
image hash. Profiled with xctrace (Time Profiler) and PE symbols. DXMT patches
0019-0027, measured at 5000 draws per frame (median of 7 interleaved runs):
app-thread time per frame 45 to 80% lower, FPS 2 to 6 times, images identical.

- 0019: dynamic buffers and textures are no longer write-combined. The game's
  x86 float code writes the mapped constant buffer, and FEX's FPCR write on the
  next call into the ARM64EC DLL stalled on the write-combined stores (83% of the
  app thread). Apple GPUs are cache coherent. `NEUTRON_DXMT_WRITE_COMBINED=1`
  restores the old mode. Base variant: 2.66 to 0.62 ms per frame.
- 0020: renames of small DEFAULT constant, vertex and index buffers
  (`UpdateSubresource`) come from page suballocations instead of a new MTLBuffer
  and a unix call each (thousands of `useResource` per frame, slow submits).
- 0021, 0022: fewer refcount operations between the app and encoder threads;
  replaced bindings are released once per chunk (`Texture::decRef` was 32% of the
  encoder thread with many bindings).
- 0023, 0027: the pipeline key hashes the blend state; a 64-entry per-context
  cache sits in front of the device's pipeline map.
- 0024: command data comes from a 64 KB span per chunk.
- 0025, 0026: unchanged input layout, depth-stencil state and blend factor emit no
  command; state setters use `static_cast` instead of a QueryInterface.
- fex 0004: FPCR, measured on M5 Max: reading costs 0.3 ns, writing the same value
  2.7 ns, writing a changed bit about 9 ns (any bit) and the write serializes. Under
  0003 a call into ARM64EC code cost 4.5 ns after integer code and 26 ns after any
  block that touched an xmm register (14-20% of the app thread). 0004 sets NEP/AH only
  in blocks that need them: data moves and int to float never, compares and float to
  int only under FTZ, and up to 8 add/sub/mul/div in a block that ends in a call that
  may reach ARM64EC code run without AH. Only NaN lanes can differ there, so each
  result is checked for NaN and a NaN lane is rebuilt with the x86 rules (first source
  if NaN, else second, else the negative default NaN) by a few vector ops and FMINNM,
  without an FPCR write (with DAZ set FMINNM would flush denormals, then AH is set and
  the op redone). The prototype set AH on a NaN instead, which clang's `fill_cb` in
  `d3d11_headless` hits every draw (it divides junk vector lanes, 0/0).
  `tests/fex_float.c bench`: call after scalar, vector, double float code, compares,
  moves and 0/0 lanes 26.5 to 4.6-5.2 ns, after sqrtss (still AFP) unchanged, pure
  float code (`cpu.exe float_sse_16k`) unchanged. `d3d11_headless` submit_ms (median
  of 7 interleaved runs): base 0.55 to 0.37, state 0.89 to 0.71, indexed, update,
  inst and deferred 17-31% lower, same images. `bind` does not gain and its FPS drops
  about 10% in most runs: the app thread then waits in its `GetData` poll and app and
  encoder thread most likely slow each other down (bistable, some runs are 5% faster;
  with a busy loop of a few ns per draw `bind` is 5% faster in every run). All 748 checks of
  `tests/fex_float.c` pass. Needs a re-audit on every FEX update: it classifies FEX's
  IR ops by name, and a new float op that is not listed runs without AH.
  `FEX_HOSTFEATURES=disableafp` makes every call cheap but scalar float chains 85%
  slower and x86 NaN results wrong.
- Left: DXMT's encoder thread at about 120 ns per draw, half of it in the AGX driver.
- Measured and not worth changing: Wine's `arm64x_check_call` costs about 0.67 ns
  per indirect call in ARM64EC code (1.33 ns against 0.66 ns for plain ARM64),
  which is the call and return the compiler emits around the check, not the TEB
  and PEB loads; caching the EC bitmap pointer moved samples but not CPU time.
  Unix calls (`__wine_unix_call_arm64ec`) save only q8-q15. NT syscalls read FPSR
  in `__wine_syscall_dispatcher` (5 ns after an FP op): storing 0 instead makes
  `QueryPerformanceCounter` 20.6 to 16.4 ns but no D3D11 benchmark moved, so it
  stays (contexts keep the real flags). QPC no longer makes the syscall, see
  QueryPerformanceCounter below.

## QueryPerformanceCounter

Wine's QPC is `mach_continuous_time()` in 100 ns units (10 MHz), and
`NtQueryPerformanceCounter` was a full NT syscall into the unix side. On macOS
`mach_continuous_time` itself needs no kernel call: libsystem reads the commpage
(`0xfffffc000`). `_COMM_PAGE_CONT_HWCLOCK` (`+0x91`) says the user-mode path exists,
`_COMM_PAGE_USER_TIMEBASE` (`+0x90`) names the counter register (3: Apple's
`ACNTVCT_EL0` = `S3_4_C15_C10_6`, 2: `CNTVCTSS_EL0`, 1: `isb` + `CNTVCT_EL0`), and
`_COMM_PAGE_CONT_HW_TIMEBASE` (`+0xa8`) is added. On the M5 Max (macOS 27) the type is
3, the timebase 125/3 (24 MHz) and the offset 0, also after the Mac had slept (6.8 h of
sleep since boot): `ACNTVCT` keeps counting while the Mac sleeps, `mach_absolute_time`
subtracts the sleep through `_COMM_PAGE_TIMEBASE_OFFSET` instead. `CNTVCT_EL0` is not the mach timebase there (`CNTFRQ_EL0` reads 1 GHz).

The pending Wine diff (`patches/wine/pending-qpc.diff`) does the same in PE code:
`RtlQueryPerformanceCounter` (which `QueryPerformanceCounter` and `timeGetTime` call)
reads the three commpage fields on every call and computes
`ticks * numer / denom / 100` exactly like the unix side. The unix side enables it per
process (page after `KUSER_SHARED_DATA`, `+0x10`) after 100 reads between two
`mach_continuous_time()` calls agreed, so an unknown commpage layout or type falls back to
the syscall; `NEUTRON_FAST_QPC=0` turns it off. Sleep and wake need no bias update: the
same fields libsystem uses are read on every call, so the values are identical to
`NtQueryPerformanceCounter`, wineserver timers and `KUSER_SHARED_DATA` at any time. A
real sleep was not tested (no way headless).

`tests/qpc.c` (x64 and ARM64EC builds): frequency, monotonic in one thread (10M reads)
and across 8 threads mixed with the syscall, QPC between two `NtQueryPerformanceCounter`
calls and the other way round (1M each), rates against `GetTickCount64` (within 17 ms,
the 16 ms KUSER update), `timeGetTime` and the system time. Cost per call (M5 Max,
best of 3):

| | before | after |
|---|---|---|
| `QueryPerformanceCounter` from x64 (FEX) | 19.2 ns | 12.8 ns |
| `QueryPerformanceCounter` from ARM64EC | 17.2 ns | 10.7 ns |
| `RtlQueryPerformanceCounter` from ARM64EC | 16.0 ns | 8.6 ns |
| `timeGetTime` from x64 | 21.2 ns | 14.3 ns |

The rest is the x64 to ARM64EC transition (about 5 ns) and `mrs ACNTVCT` (3.8 ns
natively, `mach_continuous_time` 5.3 ns). `NtQueryPerformanceCounter` called directly is
unchanged (15.5 ns from ARM64EC, 164 ns through the x64 syscall stub).

## Start time

What a game start costs besides the game. `waitforexitandrun` waits for the prefix's
wineserver to exit, so every start from Steam is a cold start: a new wineserver, then the
first process runs `wineboot --init` and waits for it. Measured with
`dev/bench/cpu.c` (`--nop`, `--user32`) through `tool/neutron runinprefix` (M5 Max,
median of 5, interleaved with the old runtime; `start.*` in `dev/bench/run.sh`):

| | before | after |
|---|---|---|
| no-op exe, cold wineserver | 573 ms | 376 ms |
| no-op exe, warm wineserver | 131 ms | 71 ms |
| cold start of a process that loads user32 and needs the desktop | 1443 ms | 632 ms |
| loading user32.dll in a process | 68 ms | 29 ms |

- **Desktop.** The first window (or `GetSystemMetrics`) starts `explorer.exe /desktop`,
  which rebuilds the display cache. win32u asked for OpenGL GPUs there, which loaded the
  macOS OpenGL driver (about 0.5 s) only to list EGL devices, and without EGL there are
  none. Skipped when Wine is built without EGL (`SONAME_LIBEGL`, never on macOS).
- **Fonts.** Every process that loads user32 enumerated the system fonts with
  `CTFontCollectionCreateMatchingFontDescriptors` (45 ms for 640 descriptors).
  `CTFontManagerCopyAvailableFontURLs` gives the same 306 files in about 10 ms; the font
  list is identical (`WINEDEBUG=+font` dump). wineboot's chain alone loads user32 in
  plugplay, winedevice (winebus) and wineboot, one after the other.
- **Tool.** `sync_file` compared the 58 MB `lsteamclient.dll` with the prefix copy byte by
  byte (45 ms) on every call; it now compares size and mtime first.
- Left in a cold start (376 ms): about 250 ms waiting for `wineboot --init` (services.exe
  starts winedevice/mountmgr, plugplay, svchost/eventlog and winedevice/winebus one after
  the other, then wineboot loads shell32, ole32 and wininet). user32 still costs 29 ms
  per process (Cocoa and raw mouse setup in winemac). Native x86 DLLs (FileAlignment
  0x200) cannot be mmapped with 16K pages and are read with `pread`; Wine's builtin DLLs
  are 64K aligned and mapped.

### FEX JIT and its code cache

`dev/bench/bigcode.py` generates an x86_64 program with 20000 different functions
(28000 FEX blocks, 6.6 MB exe) and calls each once: the first pass takes 250 to 290 ms,
the second pass 8 to 9 ms, so FEX translates about 10 us per block. A second start of the
process costs the same again: FEX keeps nothing between runs.

FEX-2610 has a work-in-progress offline code cache that also covers ARM64EC:

- `FEX_ENABLECODECACHINGWIP=1` writes a code map of the executed blocks per main
  executable to `%LOCALAPPDATA%\fex-emu\codemap\new\` and loads caches from
  `%LOCALAPPDATA%\fex-emu\cache\<name>-<fileid>-<config id>` when an image is mapped
  (`Windows/Common/ImageTracker.cpp`). The config id is a TODO (always 0), so a cache
  does not know the TSO or other code-generating settings it was built with.
- `FEXOfflineCompiler64.exe process-all` (built in `fex-arm64ec/Bin`, an ARM64EC exe
  that runs in the prefix) merges the code maps and compiles each image in a child
  process: it maps the image, compiles the listed blocks and writes code, block list and
  relocations (bigcode: 1.15 s, 25 MB).
- On ARM64EC loading is eager: the whole code is copied into an EC code buffer and
  relocated at image load (no lazy per-page mapping on Windows).

On macOS fex patch 0005 was needed to get that far: the compiler crashed on a null config
layer (it called `Logging::Init` before loading the config), it had no `MAP_JIT` write
switch (endless write fault when emitting code), and relocating or copying cached code
wrote to `MAP_JIT` memory outside a `JITWriteScope`. With it, code maps, cache generation
and cache loading work, but running the cached code does not:

- An image the compiler maps at another address than the game (every exe with the
  default base 0x140000000, where `FEXOfflineCompiler64.exe` itself sits) keeps
  RIP-relative data addresses of the compiler's mapping: the first cached block read
  `generator base + 0x4d4020`.
- With a different image base (`-Wl,--image-base=0x150000000`) the first cached block
  jumps outside every code buffer (illegal instruction at a non-JIT address).
- The ARM64EC exception handler only treats faults in `CTX` code buffers as JIT faults
  (`IsAddressInCodeBuffer`), not in loaded cache buffers, so self-modifying-code and
  unaligned-atomic handling would not work in cached code.
- `FEX_ENABLECODECACHEVALIDATION=1` crashes in the IR emitter (validation thread).

So the cache is not usable yet; what it would save is the JIT time of the blocks a
game runs at every start (about 10 us each). Needed: relocations that work when the
compiler maps the image elsewhere, the jump target bug above, the cache buffers in the
exception handler, a config id from the code-generating settings (TSO mode, AFP), and a
place in the tool to run the compiler (after a game exits) and keep the cache in
`compatdata/<appid>`.

## GPU passes

Apple GPUs are tile based: every render pass boundary loads and stores the tiles it
touches. Measured on the M5 Max: about 7 us fixed cost per render pass, a full
2924x1224 RGBA16F pass 19 us with lossless compression and 29 us without
(`MTLTextureUsagePixelFormatView` turns compression off). DXMT already folds clears
into load actions and MSAA resolves into store actions when a frame stays in one
command buffer; the waste came from frames cut into several command buffers.

- DXMT patch 0018: `GetData` no longer flushes for an event or disjoint query whose
  event is already committed, and the commit after a query End waits (at most 8
  pass boundaries) while clears are recorded whose pass has not started. Before, a
  query in the middle of a frame left its clears as separate passes and the next
  pass loaded every attachment (the "Clear passes" count in the HUD).
- 0016: `DiscardView`, `DiscardView1` (whole view) and `DiscardResource` on render
  targets and depth turn into DontCare load/store actions (MSAA: resolve only),
  using DXMT's dependency scan, so a reader in between keeps the store.
- 0017: B8G8R8A8_TYPELESS render targets without UAV keep lossless compression
  (UNORM and sRGB views do not need the pixel format view flag).
  `DXMT_CONFIG="d3d11.compressTypelessRenderTargets=True"` does it for all typeless
  render targets (opt-in per game: reading through another view type is undefined
  in Metal; `tests/d3d11_typeless_views.c` reads bit-exact on M5).
- Benchmark: `dev/bench/gpu_headless.c`, a deferred frame (shadow cascades,
  G-buffer, SSAO, lighting, bloom, TAA) with timestamp queries and image hashes;
  `dev/bench/gpu-ab.sh` compares two runtimes. With queries in the frame 4 to 6%
  less GPU time, images bit-identical in 10 configurations.
- Image hashes of `gpu_headless` can differ by 1 LSB on a few pixels when another
  process uses the GPU at the same time (10-20% of runs under GPU load, 0 under CPU
  load). That is the Apple GPU, not DXMT: implicit-LOD sampling (`Sample`, and
  `SampleGrad` with screen-space derivatives) in indexed draws now and then picks
  another mip for a few 2x2 quads; `SampleLevel` is always identical, and a native
  Metal program reproduces it without DXMT. Compare images on an otherwise idle
  GPU.

## Game controllers

Games read pads through XInput (most), DirectInput, raw HID (SDL's and Unity's HID
drivers, games with own PlayStation support) or Windows.Gaming.Input. In Wine all
four sit on winebus.sys, which runs in `winedevice.exe` and has three backends:
SDL, udev (Linux only) and IOHID (macOS). SDL maps every pad it knows to a HID
gamepad with the XInput layout (winexinput.sys then makes the XInput device, VID/PID
of the pad, name "Controller (XBOX 360 For Windows)" like on Windows); IOHID passes
the raw HID device through. winebus keeps only one of the two per pad
(`is_hidraw_enabled`): raw for DualShock 4, DualSense, Switch Pro and Joy-Cons and
a list of flight sticks and wheels, SDL for the rest.

Before: Wine was configured against whatever SDL2 headers Homebrew had (sdl2 is not
in the build's package list) and dlopens `libSDL2-2.0.0.dylib`. On a Mac without
Homebrew SDL `sdl_bus_init` logs "could not load libSDL2-2.0.0.dylib"; winebus still
expects SDL to take Xbox and generic pads and dropped their IOHID devices ("ignoring
hidraw device"), so they did not reach the game at all. PlayStation and Switch pads
came through as raw HID only, which XInput-only games do not see (Proton relies on
Steam Input for them, which the Mac client does not have for Windows games). With
Homebrew's sdl2-compat it worked by chance, with bottles built for the newest macOS
only.

Now (`wine/pending-gamepad.diff`, `build.sh`):

- SDL3 and sdl2-compat are built from pinned releases for the deployment target and
  bundled (`files/lib`: `libSDL2-2.0.0.dylib`, `libSDL3.0.dylib`, `libSDL3.dylib`
  link; sdl2-compat loads SDL3 from its own folder). configure gets their headers,
  build.sh stops when Wine was configured without SDL. SDL drives Xbox (Bluetooth
  and USB), DualShock 4, DualSense, Switch Pro and Joy-Con pairs through its HIDAPI
  drivers (IOKit), MFi and other GameController pads through GameController, and
  generic HID pads through IOKit with its controller database (unknown pads become
  DirectInput joysticks). Rumble, trigger rumble (WGI impulse triggers) and the
  macOS controller remapping (GameController pads) come with it. SDL environment
  variables (`SDL_GAMECONTROLLERCONFIG` and the `SDL_JOYSTICK_*` hints) work from
  `neutron.env`. SDL init costs about 40 ms in winedevice at prefix start.
- PlayStation and Nintendo pads go through SDL as XInput pads too.
  `NEUTRON_PAD_HIDRAW=1` gives the raw HID devices instead (Wine's default), for games
  with own support (light bar, touchpad, gyro, adaptive triggers, glyphs).
  services.exe starts winedevice with a fresh Windows environment, so the SDL bus
  reads the variable from the Unix environment (`bus_options.pad_hidraw`).
- Without libSDL2 winebus now takes the IOHID devices of all pads (SDL counts as
  disabled when it fails to load), Xbox pads get XInput from their raw HID reports.
- XInput rumble lasted 1 s: Wine's HID haptics have a 1000 ms cutoff and XInput only
  sends changes, SDL stopped the motors when the duration ran out. On Windows it
  lasts until the game changes it. The SDL backend passes duration 0 (until changed)
  for cutoffs of 1 s or more.
- `XInputGetBatteryInformation` returned `ERROR_NOT_SUPPORTED`; now wired/full for the
  gamepad (no battery report through HID), disconnected for the headset.
- `SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1`: winedevice never has the focus. SDL itself
  sets `GCController.shouldMonitorBackgroundEvents` for its GameController pads.
- Home/PS and Share buttons: macOS binds them to system gestures (Launchpad, Game
  Overlay, screenshots) and applies them for the frontmost app, which is the game,
  not winedevice. winemac.drv (`cocoa_gamepad.m`) sets
  `preferredSystemGestureState = Disabled` on every button `isBoundToSystemGesture`
  of every connected pad, so the game gets the guide button (`XInputGetStateEx`,
  WGI) and macOS does not take the game out of fullscreen.
  `NEUTRON_PAD_SYSTEM_GESTURES=1` keeps the macOS gestures.
- Button layout: sdl2-compat keeps SDL2's default `SDL_GAMECONTROLLER_USE_BUTTON_LABELS=1`,
  so on Nintendo pads the button labeled A is XInput A (by label, not by position).
  `SDL_GAMECONTROLLER_USE_BUTTON_LABELS=0` in `neutron.env` maps by position.

Testing without a pad: a virtual IOHID device (`IOHIDUserDeviceCreateWithProperties`)
needs the restricted `com.apple.developer.hid.virtual.device` entitlement: unsigned it
returns NULL, ad-hoc signed with the entitlement AMFI kills the process. So winebus
has a test hook instead: `NEUTRON_PAD_VIRTUAL=1` (or `=<vid>:<pid>`) attaches an SDL
virtual gamepad that runs a fixed button and axis sequence, echoes rumble as stick
and trigger input and unplugs itself for a second on a magic rumble value.
`tests/gamepad_list.c selftest` checks it in XInput, DirectInput, raw input and WGI,
including hotplug and rumble held over 1 s (fails without the rumble fix);
`selftest absent` with `054c:0ce6` and `NEUTRON_PAD_HIDRAW=1` checks the PlayStation
policy. Real pads: `gamepad_list.exe`, `watch` and `rumble`. (sdl2-compat 2.32.74
passes the SDL2 return value of a virtual pad's rumble callback to SDL3 as a bool, so
the hook returns 1 for success.)

## DLSS

`NEUTRON_DLSS=1` lets games turn on DLSS (super resolution), which then runs on the
MetalFX temporal scaler. The game gives DLSS its color, depth, motion vectors, jitter
and exposure every frame, which is what the temporal scaler needs, so the result is
much better than the spatial upscaling of the render scale. Opt-in per game: the game
sees an NVIDIA GPU.

What DXMT already has (built with `-Denable_nvapi=true -Denable_nvngx=true`):

- `dxgi.dll` with `DXMT_ENABLE_NVEXT=1` reports vendor 0x10DE and writes
  `HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore` `FullPath` = `C:\Windows\System32`
  (plus two keys games look at for an NVIDIA driver). Without the variable it deletes
  the values again.
- `nvapi64.dll`: the NvAPI calls games make for GPU detection (an "NVIDIA GeForce RTX
  4090", Ada AD102, driver 999.99, display and HDR queries, Reflex calls as no-ops,
  all shader extension op codes unsupported). `NvAPI_Initialize` fails without NVEXT.
  No D3D12 NvAPI calls.
- `nvngx.dll`: an NGX core. The NGX loader linked into the game loads `_nvngx.dll`
  from the `FullPath` folder and calls its D3D11 entry points; DLSS create and
  evaluate become a MetalFX temporal scaler run (`IMTLD3D11ContextExt::TemporalUpscale`,
  motion vectors at display resolution are scaled down first). The quality modes have
  fixed ratios (ultra performance 1/3, performance 1/2, balanced 0.58, quality 1/1.5,
  ultra quality 1/1.3, DLAA 1). The game's `nvngx_dlss.dll` is never loaded. No frame
  generation and no ray reconstruction.

neutron:

- `build.sh` builds both DLLs and puts them into `files/lib/neutron/dlss`, outside
  Wine's DLL folders.
- `tool/neutron` with `NEUTRON_DLSS=1` exports `DXMT_ENABLE_NVEXT=1`, copies
  `nvapi64.dll` and `nvngx.dll` (as `_nvngx.dll` and `nvngx.dll`) into the prefix's
  `system32` and sets the render scale to 1: Windows sees the native display size and
  DLSS renders at the lower resolution itself (with the render scale on, DLSS output
  would be scaled a second time). Without the setting the files are removed again
  (marker `neutron-dlss-files` in the prefix) and dxgi deletes the registry values, so
  the game sees the Apple GPU as before. The HUD shows "DLSS MetalFX temporal".
- dxmt 0033:
  - Games call the NGX parameter object through the vtable MSVC builds for
    `NVSDK_NGX_Parameter`: overloads grouped, each group in reverse declaration order
    (checked with `clang -target x86_64-pc-windows-msvc`). DXMT had the `Get` group
    partly in another order, `Get(ID3D11Resource **)` and `Get(double *)` hit each
    other's slots.
  - A render subrect of 0 (older SDKs, or games that leave it out) means the whole
    color input instead of a 0x0 MetalFX input; the size is clamped to the scaler's
    range (at most 3x below the output).
  - `EnableSignatureOverride` = 1 in both NGXCore keys: the NGX loader checks the
    NVIDIA signature of the core unless this is set, DXMT's core is not signed.
  - D3D12: the `NVSDK_NGX_D3D12_*` entry points. Evaluate records the upscale into the
    game's command list (`IMTLD3D12CommandListExt`, a new `TemporalUpscale` encoder
    type that waits for and updates the queue fence like the other passes). Motion
    vectors must be at render resolution (the `MVLowRes` flag, what Unreal and most
    engines use); display resolution ones are refused on D3D12.
  - A log line `NGX: DLSS on MetalFX temporal, <in> -> <out>, quality, flags` per
    created feature.

Findings:

- On macOS 26 and later the MetalFX temporal scaler runs a network on the Neural
  Engine (MPSGraph `ANERegion` in the backtrace). More than about 20 scaler encodes in
  one Metal command buffer end in a GPU timeout, also in a native Metal program. Games
  run DLSS once per frame and DXMT commits per Present, so only tests without Present
  hit it (`tests/dlss_ngx.c` flushes after every frame).
- Conventions, checked with the test: jitter as the offset applied to the projection
  (the sample sits at pixel center minus jitter, in render pixels) and motion vectors
  from the current to the previous position in render pixels go to MetalFX unchanged.
  Negating the jitter or the motion vectors makes the output worse than bilinear.
- `tests/dlss_ngx.c`, performance mode, 256x256 to 512x512, analytic scene (zone
  plate, checkerboard, thin lines), mean error against the exact picture (0..255):
  bilinear 37.6, DLSS after 32 still frames 18.7, after 32 scrolling frames 30.4;
  D3D11 and D3D12 the same. Render subrect 0 and no auto exposure work too.

Not tested with a game yet. Risks of the NVIDIA identity: games pick other presets
and code paths on an "RTX 4090" (ray tracing options, NVIDIA-only effects, vendor
specific workarounds), and an NVIDIA path that needs a real NVIDIA driver or NvAPI
call that is not implemented can fail. Depth as D24S8 (Depth32Float_Stencil8 in
Metal) and R11G11B10 color or output have not been tried with the scaler.

## Shader and pipeline stutter

D3D11 games hitch when they draw with a shader or render state for the first time
(new area, new effect): DXMT converts the DXBC with airconv and creates the Metal
pipeline when the first draw needs it, and its encoder thread waits for it.
Benchmark: `dev/bench/shader_stutter.c`, headless, 400 generated material-like VS/PS
pairs (skinning, normal maps, 1 to 6 textures, 1 to 8 lights, parallax loops), 720
frames paced to 60 Hz, a burst of 24 new pairs every 60 frames and one every 4
frames, every 8th pair also with 3 other blend states and in an RGBA16F pass without
depth. Frame time runs from frame start to GPU completion.

Where the time went (M5 Max, cold cache, 840 shader variants, 600 pipelines):

- `newRenderPipelineState`: 23 ms median, 15.4 s in total. airconv: 1.6 ms median,
  1.6 s in total. `newLibraryWithData` and `newFunctionWithName` 0.06 ms: Metal
  compiles AIR to GPU code only for a pipeline.
- DXMT compiled pipelines on a task pool, but added a thread only when all workers
  were running at the moment of a submit. A draw loop submits a burst before the
  workers wake up, so 24 new pairs compiled on about 4 threads: 200 ms frames.
  Metal scales to about 16 pipelines in parallel (native test with DXMT's
  metallibs: 120 pipelines in 2.3 s on 1 thread, 0.25 s on 16, the same with the
  async API); `newRenderPipelineState` is not behind a lock for ordinary pipelines
  (upstream keeps one for D3D11 tessellation and D3D12 mesh pipelines).
- Metal caches parts of a pipeline (native tests, 19 pipelines each): a new pair
  20.6 ms; with its VS compiled before in a pipeline with another PS 15.0 ms; with
  its PS compiled before with another VS 7.1 ms; both 2.9 ms. The PS part depends
  on color formats and blend state (another blend state: 18.7 ms), not on the
  depth format (0 ms). A pipeline with the VS alone does not help.
- Warm cache: airconv is skipped (DXMT's `shaders_320.db`) and Metal's cache in
  compatdata answers `newRenderPipelineState` in 0.3 ms median, so warm runs had no
  frame over budget already. A binary archive for warm starts would gain nothing
  measurable.
- Wine on macOS turns a `THREAD_PRIORITY_TIME_CRITICAL` thread into a fixed
  priority Mach thread of the highest importance. DXMT's workers use it; many busy
  workers made the game thread twice as slow in frames that created shaders.
- Creating a thread takes 1 to 2 ms in Wine; growing the pool on the game thread
  cost up to 25 ms in one call.

DXMT patch 0032:

- The pool adds a worker while queued tasks outnumber idle workers, up to the
  number of cores. Workers create the threads, the game thread at most one per
  submit. A background queue for speculative work runs on at most half the
  workers at `BELOW_NORMAL` (QoS utility); work a draw waits for keeps
  `TIME_CRITICAL`. A draw that needs a background task moves it to the front, or
  raises its worker when it already runs.
- Pipeline priming: 50 ms after a VS or PS is created and not drawn, it is compiled
  in the background in a pipeline with a partner whose signature links with it
  (the VS created next to the PS, or a shader a draw used) and the render state
  most pipelines used for as many render targets. The VS variant is exact from
  `CreateInputLayout`. When the guess is the real pipeline, the draw finds it
  compiled; otherwise only the link and the guessed-wrong part remain. Within
  50 ms a shader is compiled for its draw with foreground priority (a background
  compile started at utility QoS stays slow in MTLCompilerService). Compute
  pipelines are compiled at `CreateComputeShader`. `DXMT_CONFIG="d3d11.primePipelines
  = False"` turns priming off.
- Pixel shader variant keys drop bits that do not change the code (depth target
  bound for a PS without `SV_Depth`, unorm fix for targets the PS does not write as
  float), so such draws do not convert and compile the same function again.

Results (M5 Max, macOS 27, medians of 3 cold and 6 warm runs, alternating with the
build before; frame time in ms, "over" = frames over 16.7 ms of 720, "excess" = their
time above it, "app" = worst app thread time of a frame):

| Case | Worst frame | p99 | Over | Excess | App |
|---|---|---|---|---|---|
| load, cold, before | 214 | 113 | 149 | 2787 | 2.3 |
| load, cold, 0032 | 59 | 49 | 28 | 718 | 7.5 |
| load, warm, before | 7.7 | 4.1 | 0 | 0 | 4.2 |
| load, warm, 0032 | 8.2 | 3.0 | 0 | 0 | 3.0 |
| stream, cold, before | 213 | 113 | 149 | 2802 | 4.7 |
| stream, cold, 0032 | 89 | 78 | 149 | 2168 | 5.3 |
| stream, warm, before | 12.6 | 8.5 | 0 | 0 | 8.7 |
| stream, warm, 0032 | 13.8 | 9.7 | 0 | 0 | 9.4 |

`load` creates all shaders before the first frame, `stream` each pair in the frame
that draws it. Images are identical (`hash_rgba8`, `hash_rgba16f`), and so are the
`gpu_headless` hashes (default, `msaa`, `bgra discard`), the `d3d11_headless` numbers
(`base`, `state`, `deferred` within 2%) and the D3D11 and D3D12 tests. Of 384 primed
pipelines in a cold `load` run, 358 were the ones the draws asked for, all compiled
before the draw. Cache size after a cold run: Metal 12.8 MB (12.7 before), DXMT
6.3 MB (7.2 before).

What is left:

- A new pair costs about 20 ms of Metal compile, more than a 60 Hz frame. Shaders
  created in the frame that draws them (`stream`) still miss that frame; only the
  larger pool helps there.
- Other blend states and render target formats for a shader cannot be guessed from
  the shader; the heavy pairs' variants are most of the remaining 40 to 60 ms frames
  in `load`.
- In `stream` warm the frames take about 1 ms more CPU (p99 8.5 to 9.7 ms, still
  none over budget): conversion of each new VS at `CreateInputLayout` and the
  priming runs.
- In `load` cold the worst app thread time rises from 2.3 to 7.5 ms while the
  background compiles run (lookups of primed pipelines, CPU shared with
  MTLCompilerService).
- Priming compiles shaders a game creates but never draws (CPU in
  MTLCompilerService, at utility QoS, capped at 8192 pipelines and half the cores).
- D3D12 creates its pipelines in `CreateGraphicsPipelineState`, as the API wants,
  and still has no DXMT cache for DXIL.

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
| SDL3, sdl2-compat | 3.4.18, 2.32.74 |

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
| `wine/pending-gamepad.diff` | Game controllers: PlayStation/Nintendo pads as XInput through SDL (`NEUTRON_PAD_HIDRAW`), IOHID pads without SDL, XInput rumble held, battery, Home/Share gestures off (`NEUTRON_PAD_SYSTEM_GESTURES`), `NEUTRON_PAD_VIRTUAL` (to be split into the series) |
| `wine/pending-qpc.diff` | QPC in PE code from the commpage, no OpenGL load for the GPU list, faster font enumeration (to be split into the series) |
| `proton/0001-lsteamclient-macos` | lsteamclient on macOS: optional exports, libc++, key map, `NEUTRON_STEAMCLIENT_DYLIB` |
| `proton/0002-lsteamclient-link-libcxx` | Links lsteamclient.so against libc++ |
| `proton/0003-steam-helper-macos` | steam.exe opens `steam://` URLs with `open` |
| `fex/0001-macos-teb-tsd-and-map-jit` | TEB via TSD, `MAP_JIT` write scopes, macOS UnixLib |
| `fex/0002-host-page-guards` | Guard pages cover a whole host page |
| `fex/0003-afp-lazy-native-transition` | Fewer FPCR writes on x64 to ARM64EC calls |
| `fex/0004-afp-only-for-float-blocks` | x86 float mode (FPCR.NEP/AH) only in blocks that need it, NaN lanes before a native call fixed in software |
| `fex/0005-code-cache-map-jit` | FEX code cache tools on Apple Silicon (offline compiler config and `MAP_JIT`), not usable yet |
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
| `dxmt/0033-dlss-ngx-metalfx` | DLSS on MetalFX temporal: NGX parameter vtable, render subrect 0, signature override, D3D12 NGX |

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
- Game controllers are only tested with SDL's virtual pad so far. Open with real
  pads: Xbox over USB and other pads SDL reads through GameController (winedevice is
  a background process, SDL sets `shouldMonitorBackgroundEvents`), whether the
  disabled Home button gesture holds in native fullscreen, DualSense and Switch Pro
  rumble through SDL's HIDAPI drivers, `NEUTRON_PAD_HIDRAW=1` with a game that has own
  PlayStation support.
- DLSS (`NEUTRON_DLSS=1`) still needs a test with a real game; see DLSS.
