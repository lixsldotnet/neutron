/* neutron smoke test: D3D11 renders on Metal (through DXMT) without a window.
 * Creates a device, clears a render target to a known color, copies it to a
 * staging texture and checks the pixel. Exit code 0 means the color matched. */
#define COBJMACROS
#define INITGUID
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <stdlib.h>

#define W 64
#define H 64

static int fail(const char *what, HRESULT hr)
{
    printf("FAIL %s: hr=0x%08lx\n", what, (unsigned long)hr);
    return 1;
}

int main(void)
{
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    ID3D11Texture2D *target = NULL, *staging = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr;

    if (FAILED(hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION,
            &dev, &fl, &ctx)))
        return fail("D3D11CreateDevice", hr);
    printf("device ok, feature level 0x%x\n", fl);

    D3D11_TEXTURE2D_DESC td = { 0 };
    td.Width = W;
    td.Height = H;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(hr = ID3D11Device_CreateTexture2D(dev, &td, NULL, &target)))
        return fail("CreateTexture2D(target)", hr);
    if (FAILED(hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)target, NULL, &rtv)))
        return fail("CreateRenderTargetView", hr);

    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(hr = ID3D11Device_CreateTexture2D(dev, &td, NULL, &staging)))
        return fail("CreateTexture2D(staging)", hr);

    const float color[4] = { 0.2f, 0.4f, 0.6f, 1.0f };
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, color);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)target);

    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &map)))
        return fail("Map", hr);
    unsigned char *p = (unsigned char *)map.pData + (H / 2) * map.RowPitch + (W / 2) * 4;
    printf("pixel at center: %u %u %u %u (expect about 51 102 153 255)\n", p[0], p[1], p[2], p[3]);
    int ok = abs(p[0] - 51) <= 2 && abs(p[1] - 102) <= 2 && abs(p[2] - 153) <= 2 && p[3] == 255;
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);
    printf(ok ? "PASS D3D11 clear + copy on Metal\n" : "FAIL wrong color\n");
    return ok ? 0 : 2;
}
