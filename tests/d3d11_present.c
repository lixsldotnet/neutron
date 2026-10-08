/* neutron present test: a borderless fullscreen window like a game (macOS native
 * fullscreen through winemac.drv) with a D3D11 flip swapchain in the given format.
 * Clears to a changing color for the given seconds. Used to check which swapchains
 * macOS shows Direct (Metal HUD, NEUTRON_HUD=1) and to test DXMT's present path.
 *
 * Build: x86_64-w64-mingw32-clang -O2 -o d3d11_present.exe d3d11_present.c -ld3d11 -ldxgi -luser32
 * Run:   wine d3d11_present.exe [bgra8|rgba8|rgba8srgb|rgb10a2|rgba16f] [seconds] [scale%]
 *        scale% makes the backbuffer smaller than the window (default 100)
 */
#define COBJMACROS
#define INITGUID
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(const char *what, HRESULT hr)
{
    printf("FAIL %s: hr=0x%08lx\n", what, (unsigned long)hr);
    return 1;
}

static DXGI_FORMAT parse_format(const char *name)
{
    if (!strcmp(name, "rgba8")) return DXGI_FORMAT_R8G8B8A8_UNORM;
    if (!strcmp(name, "rgba8srgb")) return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (!strcmp(name, "rgb10a2")) return DXGI_FORMAT_R10G10B10A2_UNORM;
    if (!strcmp(name, "rgba16f")) return DXGI_FORMAT_R16G16B16A16_FLOAT;
    return DXGI_FORMAT_B8G8R8A8_UNORM;
}

int main(int argc, char **argv)
{
    const char *format_name = argc > 1 ? argv[1] : "bgra8";
    int seconds = argc > 2 ? atoi(argv[2]) : 10;
    int scale = argc > 3 ? atoi(argv[3]) : 100;
    int width = GetSystemMetrics(SM_CXSCREEN), height = GetSystemMetrics(SM_CYSCREEN);

    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "neutron_present";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("neutron_present", "neutron present test", WS_POPUP | WS_VISIBLE,
                              0, 0, width, height, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) return fail("CreateWindow", HRESULT_FROM_WIN32(GetLastError()));

    ID3D11Device *device;
    ID3D11DeviceContext *context;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION,
                                   &device, NULL, &context);
    if (FAILED(hr)) return fail("D3D11CreateDevice", hr);

    IDXGIDevice *dxgi_device;
    IDXGIAdapter *adapter;
    IDXGIFactory2 *factory;
    ID3D11Device_QueryInterface(device, &IID_IDXGIDevice, (void **)&dxgi_device);
    IDXGIDevice_GetAdapter(dxgi_device, &adapter);
    hr = IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2, (void **)&factory);
    if (FAILED(hr)) return fail("GetParent(IDXGIFactory2)", hr);

    DXGI_SWAP_CHAIN_DESC1 desc = { 0 };
    desc.Width = width * scale / 100;
    desc.Height = height * scale / 100;
    desc.Format = parse_format(format_name);
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1 *swapchain;
    hr = IDXGIFactory2_CreateSwapChainForHwnd(factory, (IUnknown *)device, hwnd, &desc, NULL, NULL, &swapchain);
    if (FAILED(hr)) return fail("CreateSwapChainForHwnd", hr);

    ID3D11Texture2D *backbuffer;
    ID3D11RenderTargetView *rtv;
    IDXGISwapChain1_GetBuffer(swapchain, 0, &IID_ID3D11Texture2D, (void **)&backbuffer);
    hr = ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)backbuffer, NULL, &rtv);
    if (FAILED(hr)) return fail("CreateRenderTargetView", hr);

    printf("window %dx%d, backbuffer %ux%u %s\n", width, height, desc.Width, desc.Height, format_name);
    fflush(stdout);

    DWORD start = GetTickCount();
    unsigned int frames = 0;
    while (GetTickCount() - start < (DWORD)seconds * 1000)
    {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
        float t = (frames % 240) / 240.0f;
        float color[4] = { t, 0.3f, 1.0f - t, 1.0f };
        ID3D11DeviceContext_OMSetRenderTargets(context, 1, &rtv, NULL);
        ID3D11DeviceContext_ClearRenderTargetView(context, rtv, color);
        hr = IDXGISwapChain1_Present(swapchain, 1, 0);
        if (FAILED(hr)) return fail("Present", hr);
        frames++;
    }
    printf("%u frames, %.1f fps\n", frames, frames * 1000.0 / (GetTickCount() - start));
    return 0;
}
