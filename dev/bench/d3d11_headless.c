/* neutron bench: D3D11 CPU overhead per call, headless (no window, no swapchain).
 *
 * x86_64 Windows program. Same engine-like draw pattern as d3d11.c, but it renders
 * into an offscreen 1280x720 render target, so it never opens a window and the
 * display can sleep. Each frame ends with Flush; a ring of 3 event queries keeps at
 * most 3 frames in flight (like a swapchain with frame latency 3).
 *
 * Variants (first argument):
 *   base     per draw Map(WRITE_DISCARD) of a constant buffer, VS/PS CB binds; every 8
 *            draws a PS and SRV switch, every 32 a blend state and VB switch (d3d11.c)
 *   update   like base, but the constant buffer is DEFAULT usage and written with
 *            UpdateSubresource per draw
 *   bind     like base, plus per draw 4 SRVs and 2 samplers rebound (PS) and 2 SRVs
 *            (VS), the binding churn of material heavy engines
 *   indexed  DrawIndexed with an index buffer, IA rebinds every 8 draws, per draw
 *            Map of the CB (closer to a mesh renderer)
 *   inst     DrawInstanced of 64 instances, per draw Map of an instance buffer
 *            (draws should be about 1/8 of the others for the same work)
 *   state    like base, but every draw sets pixel shader, blend, depth stencil and rasterizer
 *            state (16 combinations, pipeline lookups per draw) and the unchanged input layout
 *            and topology
 *   deferred base recorded on 4 deferred contexts (draws split evenly), then
 *            FinishCommandList + ExecuteCommandList on the immediate context
 *   defupdate deferred with the update pattern (UpdateSubresource of a DEFAULT constant buffer)
 *   calls    no frames: ns per call of GetType, IASetPrimitiveTopology, PSSetConstantBuffers
 *            and OMSetBlendState with unchanged arguments (transition plus DXMT entry cost), and
 *            GetType after a scalar float op (x64 SSE code before the call, as in games)
 *
 * Prints one JSON line:
 *   {"bench":"d3d11_headless","mode":..,"draws":..,"frames":..,"fps":..,"submit_ms":..,
 *    "flush_ms":..,"proc_cpu_ms":..,"submit_ns_per_draw":..,"cpu_ns_per_draw":..,
 *    "rt_hash":..,"rt_lit":..}
 * submit_ms (the draw loop on the app thread) and flush_ms are medians over the measured
 * frames, proc_cpu_ms is the CPU time of all threads of the process per frame (includes
 * DXMT's encoder thread), fps is measured frames over the wall time including the final
 * GPU wait. rt_hash is an FNV-1a hash of the render target after the last frame and
 * rt_lit the number of drawn pixels, to check that two builds render the same image
 * (same mode, draws and frames). Culling is off, so every quad is drawn.
 *
 * Build: x86_64-w64-mingw32-clang -O2 -o d3d11_headless.exe d3d11_headless.c -ld3d11 -ld3dcompiler
 * Run:   env SteamAppId=bench STEAM_COMPAT_DATA_PATH=<dir> NEUTRON_FILES=<runtime>/files \
 *           tool/neutron runinprefix d3d11_headless.exe [mode] [draws_per_frame] [frames]
 *         (defaults base 5000 300, plus 30 warmup frames)
 */
#define COBJMACROS
#define INITGUID
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH 1280
#define HEIGHT 720
#define WARMUP 30
#define LATENCY 3
#define NDEF 4

static const char shader_src[] =
    "cbuffer draw : register(b0) { float4 rect; float4 color; };\n"
    "Texture2D tex : register(t0);\n"
    "Texture2D tex1 : register(t1);\n"
    "Texture2D tex2 : register(t2);\n"
    "Texture2D tex3 : register(t3);\n"
    "SamplerState smp : register(s0);\n"
    "SamplerState smp1 : register(s1);\n"
    "struct v2p { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "v2p vs(float2 corner : POSITION)\n"
    "{\n"
    "    v2p o;\n"
    "    o.pos = float4(rect.xy + corner * rect.zw, 0.5, 1);\n"
    "    o.uv = corner;\n"
    "    return o;\n"
    "}\n"
    "v2p vs_inst(float2 corner : POSITION, float4 irect : TEXCOORD1)\n"
    "{\n"
    "    v2p o;\n"
    "    o.pos = float4(irect.xy + corner * irect.zw, 0.5, 1);\n"
    "    o.uv = corner;\n"
    "    return o;\n"
    "}\n"
    "float4 ps_solid(v2p i) : SV_Target { return color; }\n"
    "float4 ps_tex(v2p i) : SV_Target { return tex.Sample(smp, i.uv) * color; }\n"
    "float4 ps_tex4(v2p i) : SV_Target { return (tex.Sample(smp, i.uv) + tex1.Sample(smp1, i.uv) +\n"
    "    tex2.Sample(smp, i.uv) + tex3.Sample(smp1, i.uv)) * color; }\n";

struct draw_cb { float rect[4]; float color[4]; };

enum mode { M_BASE, M_UPDATE, M_BIND, M_INDEXED, M_INST, M_DEFERRED, M_STATE, M_DEFUPDATE };
static const char *mode_names[] = { "base", "update", "bind", "indexed", "inst", "deferred", "state", "defupdate" };

static const char *g_mode = "base";

static int fail(const char *what, HRESULT hr)
{
    printf("{\"bench\":\"d3d11_headless\",\"mode\":\"%s\",\"error\":\"%s hr=0x%08lx\"}\n", g_mode, what,
           (unsigned long)hr);
    return 1;
}

static ID3DBlob *compile(const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *errors = NULL;
    if (FAILED(D3DCompile(shader_src, sizeof(shader_src) - 1, "bench", NULL, NULL, entry, target, 0, 0, &code, &errors)))
    {
        if (errors) fprintf(stderr, "%s\n", (const char *)ID3D10Blob_GetBufferPointer(errors));
        return NULL;
    }
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

/* everything a draw loop needs */
static struct
{
    ID3D11RenderTargetView *rtv;
    ID3D11InputLayout *layout, *layout_inst;
    ID3D11VertexShader *vs, *vs_inst;
    ID3D11PixelShader *ps[2], *ps4;
    ID3D11Buffer *vb[2], *ib[2], *cb, *cb_default, *inst_vb;
    ID3D11ShaderResourceView *srv[6];
    ID3D11SamplerState *smp[2];
    ID3D11BlendState *blend[2];
    ID3D11RasterizerState *rs, *rs2;
    ID3D11DepthStencilState *dss[2];
} R;

static void fill_cb(struct draw_cb *c, int d, int f)
{
    float x = (float)(d % 100) / 50.0f - 1.0f, y = (float)((d / 100 + f) % 50) / 25.0f - 1.0f;
    c->rect[0] = x; c->rect[1] = y; c->rect[2] = 0.03f; c->rect[3] = 0.05f;
    c->color[0] = (float)(d & 255) / 255.0f; c->color[1] = 0.5f; c->color[2] = (float)(f & 63) / 63.0f;
    c->color[3] = 0.7f;
}

static void frame_setup(ID3D11DeviceContext *ctx, int clear)
{
    static const float clear_color[4] = { 0.05f, 0.05f, 0.08f, 1 };
    D3D11_VIEWPORT vp = { 0, 0, WIDTH, HEIGHT, 0, 1 };
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &R.rtv, NULL);
    if (clear) ID3D11DeviceContext_ClearRenderTargetView(ctx, R.rtv, clear_color);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(ctx, R.rs);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_IASetInputLayout(ctx, R.layout);
    ID3D11DeviceContext_VSSetShader(ctx, R.vs, NULL, 0);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &R.smp[0]);
}

/* the base pattern (also used by update, bind and deferred) for draws [d0, d1) */
static void draw_range(ID3D11DeviceContext *ctx, enum mode mode, int d0, int d1, int f)
{
    const UINT stride = 8, offset = 0;
    for (int d = d0; d < d1; d++)
    {
        if (!(d & 31) || d == d0) /* a deferred context starts with default state */
        {
            ID3D11DeviceContext_OMSetBlendState(ctx, R.blend[(d >> 5) & 1], NULL, 0xffffffff);
            ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &R.vb[(d >> 5) & 1], &stride, &offset);
        }
        if (!(d & 7) || d == d0)
        {
            if (mode == M_BIND)
                ID3D11DeviceContext_PSSetShader(ctx, (d >> 3) & 1 ? R.ps4 : R.ps[0], NULL, 0);
            else
                ID3D11DeviceContext_PSSetShader(ctx, R.ps[(d >> 3) & 1], NULL, 0);
            ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &R.srv[(d >> 4) & 1]);
        }
        if (mode == M_STATE)
        {
            ID3D11DeviceContext_PSSetShader(ctx, R.ps[d & 1], NULL, 0);
            ID3D11DeviceContext_OMSetBlendState(ctx, R.blend[(d >> 1) & 1], NULL, 0xffffffff);
            ID3D11DeviceContext_OMSetDepthStencilState(ctx, R.dss[(d >> 2) & 1], 0);
            ID3D11DeviceContext_RSSetState(ctx, (d >> 3) & 1 ? R.rs2 : R.rs);
            /* unchanged state set again, as engines without a state cache do */
            ID3D11DeviceContext_IASetInputLayout(ctx, R.layout);
            ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        }
        if (mode == M_BIND)
        {
            ID3D11ShaderResourceView *s4[4] = { R.srv[d % 6], R.srv[(d + 1) % 6], R.srv[(d + 2) % 6], R.srv[(d + 3) % 6] };
            ID3D11SamplerState *s2[2] = { R.smp[d & 1], R.smp[(d + 1) & 1] };
            ID3D11ShaderResourceView *v2[2] = { R.srv[(d + 4) % 6], R.srv[(d + 5) % 6] };
            ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 4, s4);
            ID3D11DeviceContext_PSSetSamplers(ctx, 0, 2, s2);
            ID3D11DeviceContext_VSSetShaderResources(ctx, 0, 2, v2);
        }
        if (mode == M_UPDATE)
        {
            struct draw_cb c;
            fill_cb(&c, d, f);
            ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)R.cb_default, 0, NULL, &c, 0, 0);
            ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &R.cb_default);
            ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &R.cb_default);
        }
        else
        {
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)R.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
            {
                fill_cb(m.pData, d, f);
                ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)R.cb, 0);
            }
            ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &R.cb);
            ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &R.cb);
        }
        ID3D11DeviceContext_Draw(ctx, 4, 0);
    }
}

static void draw_indexed(ID3D11DeviceContext *ctx, int draws, int f)
{
    const UINT stride = 8, offset = 0;
    for (int d = 0; d < draws; d++)
    {
        if (!(d & 7))
        {
            ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &R.vb[(d >> 3) & 1], &stride, &offset);
            ID3D11DeviceContext_IASetIndexBuffer(ctx, R.ib[(d >> 3) & 1], DXGI_FORMAT_R16_UINT, 0);
            ID3D11DeviceContext_PSSetShader(ctx, R.ps[(d >> 3) & 1], NULL, 0);
            ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &R.srv[(d >> 4) & 1]);
        }
        if (!(d & 31)) ID3D11DeviceContext_OMSetBlendState(ctx, R.blend[(d >> 5) & 1], NULL, 0xffffffff);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)R.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        {
            fill_cb(m.pData, d, f);
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)R.cb, 0);
        }
        ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &R.cb);
        ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &R.cb);
        ID3D11DeviceContext_DrawIndexed(ctx, 6, 0, 0);
    }
}

static void draw_inst(ID3D11DeviceContext *ctx, int draws, int f)
{
    const UINT strides[2] = { 8, 16 }, offsets[2] = { 0, 0 };
    ID3D11DeviceContext_IASetInputLayout(ctx, R.layout_inst);
    ID3D11DeviceContext_VSSetShader(ctx, R.vs_inst, NULL, 0);
    for (int d = 0; d < draws; d++)
    {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)R.inst_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        {
            float *p = m.pData;
            for (int i = 0; i < 64; i++)
            {
                p[i * 4 + 0] = (float)((d * 64 + i) % 100) / 50.0f - 1.0f;
                p[i * 4 + 1] = (float)(((d * 64 + i) / 100 + f) % 50) / 25.0f - 1.0f;
                p[i * 4 + 2] = 0.03f;
                p[i * 4 + 3] = 0.05f;
            }
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)R.inst_vb, 0);
        }
        ID3D11Buffer *vbs[2] = { R.vb[d & 1], R.inst_vb };
        ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 2, vbs, strides, offsets);
        if (!(d & 3))
        {
            ID3D11DeviceContext_PSSetShader(ctx, R.ps[(d >> 2) & 1], NULL, 0);
            ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &R.srv[(d >> 3) & 1]);
            ID3D11DeviceContext_OMSetBlendState(ctx, R.blend[(d >> 3) & 1], NULL, 0xffffffff);
        }
        D3D11_MAPPED_SUBRESOURCE mc;
        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)R.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mc)))
        {
            fill_cb(mc.pData, d, f);
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)R.cb, 0);
        }
        ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &R.cb);
        ID3D11DeviceContext_DrawInstanced(ctx, 4, 64, 0, 0);
    }
}

int main(int argc, char **argv)
{
    g_mode = argc > 1 ? argv[1] : "base";
    int draws = argc > 2 ? atoi(argv[2]) : 5000;
    int frames = argc > 3 ? atoi(argv[3]) : 300;
    enum mode mode = M_BASE;
    HRESULT hr;
    int found = 0;
    for (int i = 0; i < (int)(sizeof(mode_names) / sizeof(*mode_names)); i++)
        if (!strcmp(g_mode, mode_names[i])) { mode = i; found = 1; }
    if (!strcmp(g_mode, "calls")) found = 1;
    if (!found) return fail("unknown mode", E_INVALIDARG);

    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &fl, 1, D3D11_SDK_VERSION, &dev, NULL,
                                      &ctx)))
        return fail("D3D11CreateDevice", hr);

    D3D11_TEXTURE2D_DESC rtd = { WIDTH, HEIGHT, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT,
                                 D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    ID3D11Texture2D *rt;
    if (FAILED(hr = ID3D11Device_CreateTexture2D(dev, &rtd, NULL, &rt))) return fail("CreateTexture2D rt", hr);
    if (FAILED(hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)rt, NULL, &R.rtv)))
        return fail("CreateRenderTargetView", hr);

    ID3DBlob *vs_code = compile("vs", "vs_5_0"), *vs_inst_code = compile("vs_inst", "vs_5_0"),
             *ps_solid_code = compile("ps_solid", "ps_5_0"), *ps_tex_code = compile("ps_tex", "ps_5_0"),
             *ps_tex4_code = compile("ps_tex4", "ps_5_0");
    if (!vs_code || !vs_inst_code || !ps_solid_code || !ps_tex_code || !ps_tex4_code) return fail("D3DCompile", E_FAIL);
#define BLOB(b) ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b)
    if (FAILED(hr = ID3D11Device_CreateVertexShader(dev, BLOB(vs_code), NULL, &R.vs))) return fail("CreateVS", hr);
    if (FAILED(hr = ID3D11Device_CreateVertexShader(dev, BLOB(vs_inst_code), NULL, &R.vs_inst)))
        return fail("CreateVS inst", hr);
    if (FAILED(hr = ID3D11Device_CreatePixelShader(dev, BLOB(ps_solid_code), NULL, &R.ps[0]))) return fail("CreatePS", hr);
    if (FAILED(hr = ID3D11Device_CreatePixelShader(dev, BLOB(ps_tex_code), NULL, &R.ps[1]))) return fail("CreatePS", hr);
    if (FAILED(hr = ID3D11Device_CreatePixelShader(dev, BLOB(ps_tex4_code), NULL, &R.ps4))) return fail("CreatePS4", hr);
    D3D11_INPUT_ELEMENT_DESC ie = { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
    if (FAILED(hr = ID3D11Device_CreateInputLayout(dev, &ie, 1, BLOB(vs_code), &R.layout)))
        return fail("CreateInputLayout", hr);
    D3D11_INPUT_ELEMENT_DESC ie2[2] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
    };
    if (FAILED(hr = ID3D11Device_CreateInputLayout(dev, ie2, 2, BLOB(vs_inst_code), &R.layout_inst)))
        return fail("CreateInputLayout inst", hr);

    static const float quad[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    static const unsigned short idx[6] = { 0, 1, 2, 2, 1, 3 };
    D3D11_BUFFER_DESC bd = { sizeof(quad), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA init = { quad, 0, 0 };
    D3D11_BUFFER_DESC ibd = { sizeof(idx), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA iinit = { idx, 0, 0 };
    for (int i = 0; i < 2; i++)
    {
        if (FAILED(hr = ID3D11Device_CreateBuffer(dev, &bd, &init, &R.vb[i]))) return fail("CreateBuffer vb", hr);
        if (FAILED(hr = ID3D11Device_CreateBuffer(dev, &ibd, &iinit, &R.ib[i]))) return fail("CreateBuffer ib", hr);
    }
    D3D11_BUFFER_DESC cbd = { sizeof(struct draw_cb), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER,
                              D3D11_CPU_ACCESS_WRITE, 0, 0 };
    if (FAILED(hr = ID3D11Device_CreateBuffer(dev, &cbd, NULL, &R.cb))) return fail("CreateBuffer cb", hr);
    D3D11_BUFFER_DESC cbd2 = { sizeof(struct draw_cb), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    if (FAILED(hr = ID3D11Device_CreateBuffer(dev, &cbd2, NULL, &R.cb_default))) return fail("CreateBuffer cb2", hr);
    D3D11_BUFFER_DESC ivd = { 64 * 16, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
    if (FAILED(hr = ID3D11Device_CreateBuffer(dev, &ivd, NULL, &R.inst_vb))) return fail("CreateBuffer inst", hr);

    for (int t = 0; t < 6; t++)
    {
        static unsigned int pixels[64 * 64];
        for (int i = 0; i < 64 * 64; i++) pixels[i] = ((i ^ (i >> 6)) & 8) ? 0xffffffff : 0xff304000u + t * 0x20;
        D3D11_TEXTURE2D_DESC td = { 64, 64, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_IMMUTABLE,
                                    D3D11_BIND_SHADER_RESOURCE, 0, 0 };
        D3D11_SUBRESOURCE_DATA td_init = { pixels, 64 * 4, 0 };
        ID3D11Texture2D *tex;
        if (FAILED(hr = ID3D11Device_CreateTexture2D(dev, &td, &td_init, &tex))) return fail("CreateTexture2D", hr);
        if (FAILED(hr = ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)tex, NULL, &R.srv[t])))
            return fail("CreateShaderResourceView", hr);
    }
    for (int i = 0; i < 2; i++)
    {
        D3D11_SAMPLER_DESC smd = { i ? D3D11_FILTER_MIN_MAG_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_LINEAR,
                                   D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_WRAP,
                                   0, 1, D3D11_COMPARISON_NEVER, { 0 }, 0, D3D11_FLOAT32_MAX };
        if (FAILED(hr = ID3D11Device_CreateSamplerState(dev, &smd, &R.smp[i]))) return fail("CreateSamplerState", hr);
    }
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
        if (FAILED(hr = ID3D11Device_CreateBlendState(dev, &bld, &R.blend[i]))) return fail("CreateBlendState", hr);
    }

    if (argc > 1 && !strcmp(argv[1], "calls"))
    {
        /* per call cost of the x64 -> ARM64EC transition plus DXMT's entry */
        int n = 2000000;
        double t0 = now_ms();
        UINT s = 0;
        for (int i = 0; i < n; i++) s += ID3D11DeviceContext_GetType(ctx);
        double t1 = now_ms();
        for (int i = 0; i < n; i++) ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        double t2 = now_ms();
        for (int i = 0; i < n; i++) ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &R.cb);
        double t3 = now_ms();
        for (int i = 0; i < n; i++) ID3D11DeviceContext_OMSetBlendState(ctx, R.blend[0], NULL, 0xffffffff);
        double t4 = now_ms();
        volatile float fv = 1.0f;
        for (int i = 0; i < n; i++) { fv = fv * 1.0000001f; s += ID3D11DeviceContext_GetType(ctx); }
        double t5 = now_ms();
        printf("{\"bench\":\"d3d11_headless\",\"mode\":\"calls\",\"gettype_ns\":%.1f,\"settopology_ns\":%.1f,"
               "\"pssetcb_same_ns\":%.1f,\"omsetblend_same_ns\":%.1f,\"gettype_after_float_ns\":%.1f,\"x\":%u}\n",
               (t1 - t0) * 1e6 / n, (t2 - t1) * 1e6 / n, (t3 - t2) * 1e6 / n, (t4 - t3) * 1e6 / n, (t5 - t4) * 1e6 / n,
               s & 1);
        return 0;
    }

    /* no culling: the quads are wound counter-clockwise and would be culled as back faces */
    D3D11_RASTERIZER_DESC rsd = { D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0, 0, TRUE, FALSE, FALSE, FALSE };
    if (FAILED(hr = ID3D11Device_CreateRasterizerState(dev, &rsd, &R.rs))) return fail("CreateRasterizerState", hr);
    rsd.ScissorEnable = TRUE;
    rsd.DepthBias = 1;
    if (FAILED(hr = ID3D11Device_CreateRasterizerState(dev, &rsd, &R.rs2))) return fail("CreateRasterizerState", hr);
    for (int i = 0; i < 2; i++)
    {
        D3D11_DEPTH_STENCIL_DESC dsd = { 0 };
        dsd.DepthEnable = FALSE;
        dsd.StencilEnable = i;
        dsd.StencilReadMask = dsd.StencilWriteMask = 0xff;
        dsd.FrontFace.StencilFailOp = dsd.FrontFace.StencilDepthFailOp = dsd.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
        dsd.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
        dsd.BackFace = dsd.FrontFace;
        if (FAILED(hr = ID3D11Device_CreateDepthStencilState(dev, &dsd, &R.dss[i]))) return fail("CreateDepthStencilState", hr);
    }

    ID3D11DeviceContext *def[NDEF] = { 0 };
    if (mode == M_DEFERRED || mode == M_DEFUPDATE)
        for (int i = 0; i < NDEF; i++)
            if (FAILED(hr = ID3D11Device_CreateDeferredContext(dev, 0, &def[i]))) return fail("CreateDeferredContext", hr);

    D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
    ID3D11Query *q[LATENCY];
    for (int i = 0; i < LATENCY; i++)
        if (FAILED(hr = ID3D11Device_CreateQuery(dev, &qd, &q[i]))) return fail("CreateQuery", hr);

    double *submit_ms = malloc(frames * sizeof(double)), *flush_ms = malloc(frames * sizeof(double));
    double t_start = 0, cpu_start = 0;

    for (int f = 0; f < WARMUP + frames; f++)
    {
        if (f == WARMUP) { t_start = now_ms(); cpu_start = proc_cpu_ms(); }
        /* bound the queue: wait for the frame LATENCY back */
        if (f >= LATENCY)
            while (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)q[f % LATENCY], NULL, 0, 0) == S_FALSE)
                Sleep(0);
        double t0 = now_ms();
        if (mode == M_DEFERRED || mode == M_DEFUPDATE)
        {
            ID3D11CommandList *cl[NDEF];
            ID3D11DeviceContext_ClearRenderTargetView(ctx, R.rtv, (const float[4]){ 0.05f, 0.05f, 0.08f, 1 });
            for (int i = 0; i < NDEF; i++)
            {
                frame_setup(def[i], 0);
                draw_range(def[i], mode == M_DEFUPDATE ? M_UPDATE : M_BASE, draws * i / NDEF, draws * (i + 1) / NDEF, f);
                if (FAILED(hr = ID3D11DeviceContext_FinishCommandList(def[i], FALSE, &cl[i])))
                    return fail("FinishCommandList", hr);
            }
            for (int i = 0; i < NDEF; i++)
            {
                ID3D11DeviceContext_ExecuteCommandList(ctx, cl[i], FALSE);
                ID3D11CommandList_Release(cl[i]);
            }
        }
        else
        {
            frame_setup(ctx, 1);
            if (mode == M_INDEXED) draw_indexed(ctx, draws, f);
            else if (mode == M_INST) draw_inst(ctx, draws, f);
            else draw_range(ctx, mode, 0, draws, f);
        }
        double t1 = now_ms();
        ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)q[f % LATENCY]);
        ID3D11DeviceContext_Flush(ctx);
        double t2 = now_ms();
        if (f >= WARMUP)
        {
            submit_ms[f - WARMUP] = t1 - t0;
            flush_ms[f - WARMUP] = t2 - t1;
        }
    }
    for (int i = 0; i < LATENCY; i++)
        while (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)q[i], NULL, 0, 0) == S_FALSE) Sleep(0);
    double wall = now_ms() - t_start, cpu = proc_cpu_ms() - cpu_start;
    double sub = median(submit_ms, frames);

    /* checksum of the last frame, to compare builds (same mode, draws and frames give the same image) */
    unsigned int rt_hash = 0, rt_lit = 0;
    {
        D3D11_TEXTURE2D_DESC sd = rtd;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D *st;
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &sd, NULL, &st)))
        {
            ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)rt);
            if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m)))
            {
                rt_hash = 2166136261u;
                for (int y = 0; y < HEIGHT; y++)
                {
                    const unsigned char *row = (const unsigned char *)m.pData + (size_t)y * m.RowPitch;
                    for (int x = 0; x < WIDTH * 4; x++) rt_hash = (rt_hash ^ row[x]) * 16777619u;
                    for (int x = 0; x < WIDTH; x++) rt_lit += row[x * 4] != row[0] || row[x * 4 + 1] != row[1];
                }
                ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
            }
        }
    }

    printf("{\"bench\":\"d3d11_headless\",\"mode\":\"%s\",\"draws\":%d,\"frames\":%d,\"fps\":%.1f,\"submit_ms\":%.3f,"
           "\"flush_ms\":%.3f,\"proc_cpu_ms\":%.3f,\"submit_ns_per_draw\":%.1f,\"cpu_ns_per_draw\":%.1f,"
           "\"rt_hash\":\"%08x\",\"rt_lit\":%u}\n",
           g_mode, draws, frames, frames * 1000.0 / wall, sub, median(flush_ms, frames), cpu / frames,
           sub * 1e6 / draws, cpu / frames * 1e6 / draws, rt_hash, rt_lit);
    fflush(stdout);
    return 0;
}
