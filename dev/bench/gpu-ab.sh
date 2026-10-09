#!/bin/bash
# A/B of gpu_headless.exe between two runtimes (tool folders with `neutron` and `files/`).
#
#   dev/bench/gpu-ab.sh <runtime-A> <runtime-B> [runs] [frames] [bench options...]
#
# Runs A and B alternately (other GPU load hits both alike), prints the median GPU ms per frame,
# the median of the per-run 10th percentile, and whether the final images match. With
# GPU_AB_DUMP=1 the last run of each side writes its readbacks and they are compared per pixel.
# GPU_AB_ENV_A / GPU_AB_ENV_B: extra environment for one side (e.g. DXMT_CONFIG=...).
# Each runtime gets its own prefix under .native-build/opt/gpu-ab/.
set -euo pipefail
A="$1"; B="$2"; RUNS="${3:-5}"; FRAMES="${4:-600}"; shift 2; shift $(( $# > 0 ? 1 : 0 )); shift $(( $# > 0 ? 1 : 0 ))
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OPT="$ROOT/.native-build/opt/gpu-ab"
EXE="$OPT/gpu_headless.exe"
CC="${BENCH_CC:-$(ls -d "$ROOT"/.native-build/toolchains/llvm-mingw-*/bin/x86_64-w64-mingw32-clang | tail -1)}"
mkdir -p "$OPT/A" "$OPT/B"
[ "$EXE" -nt "$ROOT/dev/bench/gpu_headless.c" ] || "$CC" -O2 -o "$EXE" "$ROOT/dev/bench/gpu_headless.c" -ld3d11 -ld3dcompiler
OUT="$OPT/ab-$(date +%Y%m%d-%H%M%S).jsonl"

run() { # side runtime extra-env args...
  local side="$1" rt="$2" extra="$3"; shift 3
  env SteamAppId=gpuab STEAM_COMPAT_DATA_PATH="$OPT/$side" NEUTRON_FILES="$rt/files" $extra \
    "$rt/neutron" runinprefix "$EXE" "$@" 2>/dev/null | tr -d '\r' | grep '^{'
}

for i in $(seq 1 "$RUNS"); do
  dump=()
  if [ "${GPU_AB_DUMP:-0}" = 1 ] && [ "$i" = "$RUNS" ]; then dump=(dump); fi
  for side in A B; do
    if [ $side = A ]; then rt="$A"; extra="${GPU_AB_ENV_A:-}"; else rt="$B"; extra="${GPU_AB_ENV_B:-}"; fi
    d=(); [ ${#dump[@]} = 1 ] && d=("dump=Z:$OPT/$side-last")
    r=$(run $side "$rt" "$extra" "$FRAMES" "$@" ${d[@]+"${d[@]}"})
    echo "{\"side\":\"$side\",\"r\":$r}" >> "$OUT"
  done
done

python3 -I - "$OUT" "$OPT" "${GPU_AB_DUMP:-0}" <<'PY'
import json, os, statistics as st, sys
rows = [json.loads(l) for l in open(sys.argv[1])]
med = {}
for side in ("A", "B"):
    v = [x["r"] for x in rows if x["side"] == side]
    g, p = st.median(r["gpu_ms"] for r in v), st.median(r["gpu_p10_ms"] for r in v)
    med[side] = (g, p)
    print(f"{side}: gpu_ms {g:.3f}  p10 {p:.3f}  wall_ms {st.median(r['wall_ms'] for r in v):.3f}  "
          f"runs {' '.join('%.3f' % r['gpu_ms'] for r in v)}  ldr {sorted(set(r['hash_ldr'] for r in v))}")
print(f"B vs A: median {100 * (med['B'][0] / med['A'][0] - 1):+.1f}%  p10 {100 * (med['B'][1] / med['A'][1] - 1):+.1f}%")
if sys.argv[3] == "1":
    for name, bpp in (("ldr", 4), ("hdr", 8), ("depth", 8)):
        fa, fb = f"{sys.argv[2]}/A-last.{name}.raw", f"{sys.argv[2]}/B-last.{name}.raw"
        if not (os.path.exists(fa) and os.path.exists(fb)):
            continue
        a, b = open(fa, "rb").read(), open(fb, "rb").read()
        if a == b:
            print(f"{name}: identical")
            continue
        n = sum(1 for i in range(0, len(a), bpp) if a[i:i + bpp] != b[i:i + bpp])
        print(f"{name}: {n} pixels differ")
PY
