# Benchmarks

Tools to measure a change before it goes in. Everything runs through
`tool/neutron`, with the same environment, FEX registration and TSO default as a
start from Steam.

| File | What |
|---|---|
| `run.sh` | Runs the suite and prints one JSON result, appended to `.native-build/opt/bench-history.jsonl` |
| `cpu.c` | CPU and Windows API micro benchmarks (x86 code in FEX, memory, atomics, wineserver round trips, clocks, virtual memory, heap, threads, calls into ARM64EC DLLs) |
| `d3d11.c` | D3D11 CPU overhead: many small draws per frame like an engine, no vsync |
| `bench.py` | Helper: wraps program output, follows game logs, summarizes, makes baselines, compares |
| `hang-watch.sh` | Starts a game and dumps it (sample, vmmap, lldb backtraces) when its FPS log stalls |

## Running

```sh
dev/bench/run.sh                      # micro benches, 5 runs each
dev/bench/run.sh --games              # plus the game profiles
dev/bench/run.sh --game gwyfs         # one game profile only
dev/bench/bench.py baseline a.json b.json > baseline.json
dev/bench/run.sh --games --compare baseline.json
```

`run.sh` uses the installed tool's runtime unless `NEUTRON_FILES` points to
another one, and the llvm-mingw clang from `./build.sh` unless `BENCH_CC` is set.
The game profiles need the games installed in the default Steam library.

## Metrics

Metric names carry their unit (`_ns`, `_ms`, `_s`, `fps`, `_cores`). Everything
except `fps` is lower-is-better. Each metric is the median of its runs, with the
spread in `spread_pct`.

- `cpu.*`: ns per operation, FEX fast TSO. `cpu_strict.*`: the TSO-sensitive
  tests again with `NEUTRON_FEX_TSO=strict`.
- `start.nop_exe_ms`: wall time to start a no-op exe through the tool, warm
  wineserver.
- `d3d11_5k.*`: 5000 draws per frame, draw loop time and process CPU time per
  frame. Its FPS sits at the display refresh and is left out.
- `d3d11_20k.*`: 20000 draws per frame, CPU bound, so its FPS is a CPU number too.
- `gwyf.*`, `af.*`: Gamble With Your Friends and Abiotic Factor main menu. From the
  FPS log (one line per second), 20 lines warmup, median over the next 40.
  `first_fps_s` is the time from launch to the first FPS line, `menu_s` the time to
  steady rendering, `cpu_per_frame_ms` the CPU time of the game process per frame,
  `busy_cores` that CPU time over wall time, `wall_ms_per_frame` 1000 / median FPS.
- `gwyfs.*`: Gamble with strict TSO and `NEUTRON_BENCH_NOVSYNC=1`, see below.

`bench.py baseline` merges several full results into a baseline with a noise
estimate per metric (run-to-run difference, at least half the spread inside one
run, at least 0.5%). `--compare` marks a change as better or worse only when it
is larger than twice the noise.

## Conditions

- **No HUD.** `NEUTRON_HUD=2` costs about 11% game CPU per frame in the Gamble
  menu (4.56 ms without, 5.09 ms with, measured on M5 Max, macOS 27). `run.sh`
  unsets `NEUTRON_HUD` and `MTL_HUD_*` and records them as `meta.hud_unset`;
  `BENCH_KEEP_HUD=1` keeps them.
- **Uncapped profile.** The Gamble menu runs at the display refresh (160 Hz on the
  test machine), also with strict TSO, so FPS hides most CPU gains.
  `NEUTRON_BENCH_NOVSYNC=1` (DXMT patch `0007-bench-novsync`, never set by the
  tool) draws the present pass into an offscreen texture and skips
  `nextDrawable` and present: `Present(0)` and `displaySyncEnabled = NO` do not
  lift the compositor's drawable pacing. The `gwyfs` profile combines it with
  strict TSO and gives about 464 FPS with a spread of 6%. The menu is light, strict
  TSO costs only about 3% there (464 against 476 FPS fast), so TSO changes are
  hard to see in it; the in-game view cannot be driven by a script.
- **One game at a time.** Game profiles are skipped when another game runs, and
  the D3D11 benches too: a game in front makes macOS run the bench window 2 to 3
  times slower. The result records running Wine and game processes and the load
  average.
- **Recorded environment.** `meta` holds the git revision (`+dirty` with local
  changes), the runtime path and the mtime of its `ntdll.so`, and every
  `NEUTRON_*`, `FEX_*`, `DXMT_*`, `WINE*` and `MTL_*` variable set by the caller.

## hang-watch.sh

```sh
dev/bench/hang-watch.sh 427410 af-test 120
```

Starts the game (Abiotic Factor 427410 or Gamble With Your Friends 3892270) with
the FPS log on. When the game rendered once and then logs no FPS line for 20 s,
it writes one dump to `.native-build/opt/hang/<time>-<label>` (`HANG_OUT`
changes the folder, `HANG_DUMP=0` turns dumps off). It ends after the given
time, a 30 s stall or 12 FPS lines and prints a `RESULT` line with the number of
FPS lines, elapsed time and last gap.
