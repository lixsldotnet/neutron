/* neutron smoke test: D3D11 presents into a real window (DXMT -> CAMetalLayer of the
 * winemac.drv view). Clears to a changing color for 300 frames and prints the frame rate.
 * Exit code 0 means every Present succeeded. */
#define COBJMACROS
#define INITGUID
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>

#define FRAMES 300

static int fail(const char *what, HRESULT hr)
{
    printf("FAIL %s: hr=0x%08lx\n", what, (unsigned long)hr);
    return 1;
}

int main(void)
{
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "neutron_d3d11";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("neutron_d3d11", "neutron D3D11 window test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              100, 100, 640, 480, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) return fail("CreateWindow", HRESULT_FROM_WIN32(GetLastError()));

    DXGI_SWAP_CHAIN_DESC sd = { 0 };
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 640;
    sd.BufferDesc.Height = 480;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    IDXGISwapChain *swap;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                                               D3D11_SDK_VERSION, &sd, &swap, &dev, NULL, &ctx);
    if (FAILED(hr)) return fail("D3D11CreateDeviceAndSwapChain", hr);

    ID3D11Texture2D *back;
    ID3D11RenderTargetView *rtv;
    if (FAILED(hr = IDXGISwapChain_GetBuffer(swap, 0, &IID_ID3D11Texture2D, (void **)&back)))
        return fail("GetBuffer", hr);
    if (FAILED(hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)back, NULL, &rtv)))
        return fail("CreateRenderTargetView", hr);

    LARGE_INTEGER freq, start, end;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    for (int i = 0; i < FRAMES; i++)
    {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
        float t = (float)i / FRAMES;
        const float color[4] = { t, 0.4f, 1.0f - t, 1.0f };
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, color);
        if (FAILED(hr = IDXGISwapChain_Present(swap, 1, 0))) return fail("Present", hr);
    }
    QueryPerformanceCounter(&end);
    double secs = (double)(end.QuadPart - start.QuadPart) / freq.QuadPart;
    printf("PASS %d frames presented in %.2f s (%.1f fps, vsync on)\n", FRAMES, secs, FRAMES / secs);
    return 0;
}
