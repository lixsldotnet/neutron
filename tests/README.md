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
