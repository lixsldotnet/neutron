# tests

Small Windows programs that check one part of the stack each. They are built
as x86_64 Windows executables, like games, so they also run through FEX.

| Test | Checks | Window | Libraries |
|---|---|---|---|
| `smoke.c` | `lsteamclient.dll` loads and reaches the Mac `steamclient.dylib` (`CreateSteamPipe` is 0 when Steam does not run) | no | |
| `d3d11_clear.c` | D3D11 device on Metal, clear and readback of one pixel | no | `-ld3d11` |
| `d3d11_window.c` | D3D11 swapchain in a real window, 300 frames, prints the frame rate | yes | `-ld3d11 -luser32` |
| `d3d11_present.c` | borderless fullscreen window with a flip swapchain in a given format (see `dev/direct-test.sh`) | yes, fullscreen | `-ld3d11 -ldxgi -luser32` |
| `d3d12_clear.c` | D3D12 device, clear and readback of one pixel | no | `-ld3d12` |
| `d3d12_triangle.c` | D3D12 root signature, pipeline with HLSL shaders, one draw, pixel check | no | `-ld3d12 -ld3dcompiler` |
| `d3d12_caps.c` | D3D12 caps the way Unreal 5 checks them (feature level, shader model, binding tier, wave ops, 64-bit atomics); exit 0 when Unreal would pick SM6 | no | `-ldxgi -ld3d12 -lole32` |
| `d3d12_dxil_compute.c` | DXIL compute: groupshared, barriers, cbuffer, root UAVs, wave ops (needs `DXMT_CONFIG="d3d12.shaderModel = 66"`, as all `d3d12_dxil_*`) | no | `-ld3d12 -ldxgi` |
| `d3d12_dxil_draw.c` | DXIL VS and PS, input layout, SRV table, static sampler, discard | no | `-ld3d12 -ldxgi` |
| `d3d12_dxil_mrt.c` | DXIL with 3 render targets, SV_Depth, IsFrontFace, InstanceID | no | `-ld3d12` |
| `d3d12_dxil_ops.c` | DXIL operations: GetDimensions, Gather, offsets, bit ops, wave and quad ops, f16 conversion, trig | no | `-ld3d12` |
| `d3d12_dxil_heap.c` | SM 6.6 descriptor heap indexing | no | `-ld3d12` |
| `d3d12_dxil_atomic64.c` | 64-bit InterlockedMax/Min like Nanite | no | `-ld3d12` |
| `d3d12_dxil_pso.c` | pipeline creation for 12 pixel shaders (derivatives, LOD, MSAA, coverage, UAV, wave) | no | `-ld3d12` |
| `d3d12_dxil_gs.c` | DXIL geometry shaders: the `d3d12_gs.c` cases plus GS instancing (`[instance(2)]`, SV_GSInstanceID) | no | `-ld3d12` |
| `d3d12_dxil_tess.c` | DXIL hull and domain shaders: the `d3d12_tess.c` cases, the patch constant function reads an output control point | no | `-ld3d12` |
| `d3d12_gs.c` | geometry shaders (DXBC): point to quad, strips, indexed 16/32-bit with base vertex, instancing, line input with an SRV, render target array index, root parameters visible to the GS only, mixed with ordinary draws | no | `-ld3d12 -ld3dcompiler` |
| `d3d12_tess.c` | tessellation (DXBC): tri and quad domains, patch constants, a displaced edge that only shows when the patch is tessellated, factors from root constants, indexed and instanced patches | no | `-ld3d12 -ld3dcompiler` |
| `d3d11_typeless_views.c` | typeless render targets written through one view type and read through another (lossless compression check) | no | `-ld3d11 -ld3dcompiler` |
| `fex_float.c` | 748 checks of x86 SSE float results under FEX, alone, after and right before a call into ARM64EC code (NaN sign and propagation, min/max, DAZ/FTZ, compares, conversions, rcp/rsqrt, upper lanes of scalar ops); `bench` also prints the cost of such a call after integer, float and vector code | no | |
| `sync_semantics.c` | 56 checks of Windows sync semantics (wait any/all, APCs, abandoned mutexes, cross-process), with and without `NEUTRON_MSYNC=1` | no | |
| `sandbox/sbtest.sh` | the game sandbox (`NEUTRON_SANDBOX=1`): 35 checks of what is denied and allowed, D3D11, audio and controllers inside it | no | builds its own |

Exit code 0 means the test passed.

## Build

With the llvm-mingw toolchain that `./build.sh` fetches:

```sh
CC="$(ls -d .native-build/toolchains/llvm-mingw-*/bin/x86_64-w64-mingw32-clang | tail -1)"
mkdir -p .native-build/opt/tests
"$CC" -O2 -o .native-build/opt/tests/d3d11_clear.exe tests/d3d11_clear.c -ld3d11
```

Use the libraries from the table for the other tests.

## Run

Through `tool/neutron runinprefix`, in an own prefix, with the runtime of the
installed tool (or a build's `files` folder):

```sh
export NEUTRON_FILES="$HOME/Library/Application Support/neutron/compatibilitytools.d/neutron_proton/files"
export STEAM_COMPAT_DATA_PATH="$PWD/.native-build/opt/testdata" SteamAppId=tests
mkdir -p "$STEAM_COMPAT_DATA_PATH"
tool/neutron runinprefix .native-build/opt/tests/d3d11_clear.exe; echo "exit $?"
```

The first run creates the prefix and takes longer. The tool's log is
`~/Library/Logs/neutron/neutron-tests.log`. `smoke.exe` needs a running,
logged-in Mac Steam for a non-zero pipe. `d3d11_present.exe` takes
`[bgra8|rgba8|rgba8srgb|rgb10a2|rgba16f] [seconds] [scale%]`; set
`NEUTRON_HUD=1` to see whether macOS presents it Direct or Composited.
