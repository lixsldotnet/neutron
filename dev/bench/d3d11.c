/* neutron bench: D3D11 CPU overhead (DXMT on Metal + Wine), independent of games.
 *
 * x86_64 Windows program. Opens a 1280x720 window and renders frames with many small
 * draw calls the way engines do: per draw a Map(WRITE_DISCARD) of a constant buffer
 * and constant buffer binds, every 8 draws a shader and texture switch, every 32 draws
 * a blend state and vertex buffer switch. Present without vsync, so the frame rate
 * is bound by the CPU cost of the D3D11 calls (the GPU work is tiny).
 * Prints one JSON line:
 *   {"bench":"d3d11","draws":5000,"frames":300,"fps":..,"frame_ms":..,"submit_ms":..,
 *    "present_ms":..,"proc_cpu_ms":..}
 * frame_ms, submit_ms and present_ms are medians over the measured frames (submit is
 * the draw loop, present the Present call), proc_cpu_ms is the CPU time of all
 * threads of the process per frame (includes DXMT's encoder thread), fps is
 * measured frames over the wall time including the final GPU wait.
 *
 * Build: x86_64-w64-mingw32-clang -O2 -o d3d11.exe d3d11.c -ld3d11 -ld3dcompiler -lgdi32 -luser32
 * Run:   wine d3d11.exe [draws_per_frame] [frames]   (defaults 5000 300, plus 30 warmup)
 */
#define COBJMACROS
#define INITGUID
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>

#define WIDTH 1280
#define HEIGHT 720
#define WARMUP 30

static const char shader_src[] =
    "cbuffer draw : register(b0) { float4 rect; float4 color; };\n"
    "Texture2D tex : register(t0);\n"
    "SamplerState smp : register(s0);\n"
    "struct v2p { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "v2p vs(float2 corner : POSITION)\n"
    "{\n"
    "    v2p o;\n"
    "    o.pos = float4(rect.xy + corner * rect.zw, 0.5, 1);\n"
    "    o.uv = corner;\n"
    "    return o;\n"
    "}\n"
    "float4 ps_solid(v2p i) : SV_Target { return color; }\n"
    "float4 ps_tex(v2p i) : SV_Target { return tex.Sample(smp, i.uv) * color; }\n";

struct draw_cb { float rect[4]; float color[4]; };

static int fail(const char *what, HRESULT hr)
{
    printf("{\"bench\":\"d3d11\",\"error\":\"%s hr=0x%08lx\"}\n", what, (unsigned long)hr);
    return 1;
}

static ID3DBlob *compile(const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *errors = NULL;
    if (FAILED(D3DCompile(shader_src, sizeof(shader_src) - 1, "bench", NULL, NULL, entry, target, 0, 0, &code, &errors)))
        return NULL;
    return code;
}

static double now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

static double proc_cpu_ms(void)
{
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    ULARGE_INTEGER kk = { .LowPart = k.dwLowDateTime, .HighPart = k.dwHighDateTime };
    ULARGE_INTEGER uu = { .LowPart = u.dwLowDateTime, .HighPart = u.dwHighDateTime };
    return (double)(kk.QuadPart + uu.QuadPart) / 10000.0;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static double median(double *v, int n)
{
    qsort(v, n, sizeof(*v), cmp_double);
    return n & 1 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

int main(int argc, char **argv)
{
    int draws = argc > 1 ? atoi(argv[1]) : 5000;
    int frames = argc > 2 ? atoi(argv[2]) : 300;
    HRESULT hr;

    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "neutron_bench_d3d11";
    RegisterClassA(&wc);
    RECT r = { 0, 0, WIDTH, HEIGHT };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowA(wc.lpszClassName, "neutron bench d3d11", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 60, 60,
                              r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) return fail("CreateWindow", HRESULT_FROM_WIN32(GetLastError()));

    DXGI_SWAP_CHAIN_DESC sd = { 0 };
    sd.BufferCount = 2;
    sd.BufferDesc.Width = WIDTH;
    sd.BufferDesc.Height = HEIGHT;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    IDXGISwapChain *swap;
    if (FAILED(hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION,
                                                  &sd, &swap, &dev, NULL, &ctx)))
        return fail("D3D11CreateDeviceAndSwapChain", hr);

    ID3D11Texture2D *back;
    ID3D11RenderTargetView *rtv;
    if (FAILED(hr = IDXGISwapChain_GetBuffer(swap, 0, &IID_ID3D11Texture2D, (void **)&back))) return fail("GetBuffer", hr);
    if (FAILED(hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)back, NULL, &rtv)))
        return fail("CreateRenderTargetView", hr);

    ID3DBlob *vs_code = compile("vs", "vs_5_0"), *ps_solid_code = compile("ps_solid", "ps_5_0"),
             *ps_tex_code = compile("ps_tex", "ps_5_0");
    if (!vs_code || !ps_solid_code || !ps_tex_code) return fail("D3DCompile", E_FAIL);
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps[2];
    ID3D11InputLayout *layout;
    if (FAILED(hr = ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vs_code),
                                                    ID3D10Blob_GetBufferSize(vs_code), NULL, &vs)))
        return fail("CreateVertexShader", hr);
    if (FAILED(hr = ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(ps_solid_code),
                                                   ID3D10Blob_GetBufferSize(ps_solid_code), NULL, &ps[0])))
        return fail("CreatePixelShader", hr);
    if (FAILED(hr = ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(ps_tex_code),
                                                   ID3D10Blob_GetBufferSize(ps_tex_code), NULL, &ps[1])))
        return fail("CreatePixelShader", hr);
    D3D11_INPUT_ELEMENT_DESC ie = { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
    if (FAILED(hr = ID3D11Device_CreateInputLayout(dev, &ie, 1, ID3D10Blob_GetBufferPointer(vs_code),
                                                   ID3D10Blob_GetBufferSize(vs_code), &layout)))
        return fail("CreateInputLayout", hr);

    /* two vertex buffers with the same quad (switching them is the state change) */
    static const float quad[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    D3D11_BUFFER_DESC bd = { sizeof(quad), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA init = { quad, 0, 0 };
    ID3D11Buffer *vb[2], *cb;
    for (int i = 0; i < 2; i++)
        if (FAILED(hr = ID3D11Device_CreateBuffer(dev, &bd, &init, &vb[i]))) return fail("CreateBuffer vb", hr);
    D3D11_BUFFER_DESC cbd = { sizeof(struct draw_cb), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER,
                              D3D11_CPU_ACCESS_WRITE, 0, 0 };
    if (FAILED(hr = ID3D11Device_CreateBuffer(dev, &cbd, NULL, &cb))) return fail("CreateBuffer cb", hr);

    /* two small textures */
    ID3D11ShaderResourceView *srv[2];
    for (int t = 0; t < 2; t++)
    {
        static unsigned int pixels[64 * 64];
        for (int i = 0; i < 64 * 64; i++) pixels[i] = ((i ^ (i >> 6)) & 8) ? 0xffffffff : (t ? 0xff3080ff : 0xff40c040);
        D3D11_TEXTURE2D_DESC td = { 64, 64, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_IMMUTABLE,
                                    D3D11_BIND_SHADER_RESOURCE, 0, 0 };
        D3D11_SUBRESOURCE_DATA td_init = { pixels, 64 * 4, 0 };
        ID3D11Texture2D *tex;
        if (FAILED(hr = ID3D11Device_CreateTexture2D(dev, &td, &td_init, &tex))) return fail("CreateTexture2D", hr);
        if (FAILED(hr = ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)tex, NULL, &srv[t])))
            return fail("CreateShaderResourceView", hr);
    }
    D3D11_SAMPLER_DESC smd = { D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_WRAP,
                               D3D11_TEXTURE_ADDRESS_WRAP, 0, 1, D3D11_COMPARISON_NEVER, { 0 }, 0, D3D11_FLOAT32_MAX };
    ID3D11SamplerState *smp;
    if (FAILED(hr = ID3D11Device_CreateSamplerState(dev, &smd, &smp))) return fail("CreateSamplerState", hr);

    ID3D11BlendState *blend[2];
    for (int i = 0; i < 2; i++)
    {
        D3D11_BLEND_DESC bld = { 0 };
        bld.RenderTarget[0].BlendEnable = i;
        bld.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(hr = ID3D11Device_CreateBlendState(dev, &bld, &blend[i]))) return fail("CreateBlendState", hr);
    }

    D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
    ID3D11Query *done;
    if (FAILED(hr = ID3D11Device_CreateQuery(dev, &qd, &done))) return fail("CreateQuery", hr);

    double *frame_ms = malloc(frames * sizeof(double)), *submit_ms = malloc(frames * sizeof(double)),
           *present_ms = malloc(frames * sizeof(double));
    D3D11_VIEWPORT vp = { 0, 0, WIDTH, HEIGHT, 0, 1 };
    const UINT stride = 8, offset = 0;
    const float clear[4] = { 0.05f, 0.05f, 0.08f, 1 };
    double t_start = 0, cpu_start = 0;

    for (int f = 0; f < WARMUP + frames; f++)
    {
        if (f == WARMUP) { t_start = now_ms(); cpu_start = proc_cpu_ms(); }
        double t0 = now_ms();
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);

        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, clear);
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ID3D11DeviceContext_IASetInputLayout(ctx, layout);
        ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
        ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &smp);
        for (int d = 0; d < draws; d++)
        {
            if (!(d & 31))
            {
                ID3D11DeviceContext_OMSetBlendState(ctx, blend[(d >> 5) & 1], NULL, 0xffffffff);
                ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb[(d >> 5) & 1], &stride, &offset);
            }
            if (!(d & 7))
            {
                ID3D11DeviceContext_PSSetShader(ctx, ps[(d >> 3) & 1], NULL, 0);
                ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &srv[(d >> 4) & 1]);
            }
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
            {
                struct draw_cb *c = m.pData;
                float x = (float)(d % 100) / 50.0f - 1.0f, y = (float)((d / 100 + f) % 50) / 25.0f - 1.0f;
                c->rect[0] = x; c->rect[1] = y; c->rect[2] = 0.03f; c->rect[3] = 0.05f;
                c->color[0] = (float)(d & 255) / 255.0f; c->color[1] = 0.5f; c->color[2] = (float)(f & 63) / 63.0f;
                c->color[3] = 0.7f;
                ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)cb, 0);
            }
            ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cb);
            ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cb);
            ID3D11DeviceContext_Draw(ctx, 4, 0);
        }
        double t1 = now_ms();
        if (FAILED(hr = IDXGISwapChain_Present(swap, 0, 0))) return fail("Present", hr);
        double t2 = now_ms();
        if (f >= WARMUP)
        {
            submit_ms[f - WARMUP] = t1 - t0;
            present_ms[f - WARMUP] = t2 - t1;
            frame_ms[f - WARMUP] = t2 - t0;
        }
    }
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)done);
    ID3D11DeviceContext_Flush(ctx);
    while (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)done, NULL, 0, 0) == S_FALSE) Sleep(0);
    double wall = now_ms() - t_start, cpu = proc_cpu_ms() - cpu_start;

    printf("{\"bench\":\"d3d11\",\"draws\":%d,\"frames\":%d,\"fps\":%.1f,\"frame_ms\":%.3f,\"submit_ms\":%.3f,"
           "\"present_ms\":%.3f,\"proc_cpu_ms\":%.3f}\n",
           draws, frames, frames * 1000.0 / wall, median(frame_ms, frames), median(submit_ms, frames),
           median(present_ms, frames), cpu / frames);
    fflush(stdout);
    DestroyWindow(hwnd);
    return 0;
}
