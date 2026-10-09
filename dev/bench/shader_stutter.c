/* neutron bench: D3D11 frame hitches when shaders and pipelines are used for the first time.
 *
 * x86_64 Windows program, headless (offscreen targets, no window, no swapchain). Generates
 * <pairs> distinct VS/PS pairs in HLSL like an engine's materials (skinned and static vertex
 * formats, normal mapping, 1 to 6 textures, 1 to 8 unrolled lights, parallax loops, fog,
 * extra interpolants), compiles them to DXBC before the clock starts (games ship DXBC), then
 * renders frames paced to 60 Hz. New pairs come into use over time: a burst of 24 every
 * 60 frames ("new area") and one every 4 frames in between ("new effect"). Every 8th pair
 * is a heavy material with variants: 4 blend states (new Metal pipelines only) and a second
 * pass into an RGBA16F target without depth (new pixel shader variant in DXMT, plus pipeline).
 *
 * Frame: pass 1 into RGBA8 + D24S8, pass 2 into RGBA16F, End(event query), Flush, wait for
 * the query (frame latency 1, so a frame's time includes DXMT's encoder thread and the GPU,
 * like the present interval a player sees). Then sleep to the next 16.67 ms tick.
 *
 * Modes (first argument):
 *   load    all shaders and input layouts are created before the first frame (level load);
 *           create_ms is that time
 *   stream  each pair is created in the frame that first draws it (streaming engines)
 *
 * Usage: shader_stutter.exe [load|stream] [pairs (default 400)] [frames (default 720)] [fps (60, 0 = no pacing)]
 * Prints one JSON line:
 *   {"bench":"shader_stutter","mode":..,"pairs":..,"frames":..,"create_ms":..,"frame_ms_med":..,
 *    "frame_ms_p99":..,"frame_ms_max":..,"submit_ms_max":..,"over_budget":..,"excess_ms":..,
 *    "proc_cpu_ms":..,"draws_last":..,"hash_rgba8":..,"hash_rgba16f":..}
 * frame_ms is from the frame start to the GPU completing it; over_budget counts frames longer
 * than the pacing budget (16.67 ms), excess_ms sums their time above it (the visible stall).
 * submit_ms_max is the worst time on the app thread alone (draw calls and Create* in stream).
 * The first 16 pairs are drawn in 30 unmeasured warmup frames (a loading screen).
 * SHADER_STUTTER_CSV=<windows path> writes frame,frame_ms,submit_ms,new_pairs per frame.
 *
 * Cold and warm: remove <compatdata>/dxmt-cache for a cold run, run again for a warm one.
 * Build: x86_64-w64-mingw32-clang -O2 -o shader_stutter.exe shader_stutter.c -ld3d11 -ld3dcompiler
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
#define INITIAL 16
#define NTEX 6
#define NBLEND 4
#define STRIDE 56

static const char *g_mode = "load";

static int fail(const char *what, HRESULT hr)
{
    printf("{\"bench\":\"shader_stutter\",\"mode\":\"%s\",\"error\":\"%s hr=0x%08lx\"}\n", g_mode, what,
           (unsigned long)hr);
    fflush(stdout);
    return 1;
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

/* ---- shader generation ---- */

struct pair_desc
{
    int skinned, tangent, extra, ntex, nlights, parallax, fog, heavy;
};

static unsigned int rng(unsigned int *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

static struct pair_desc describe(int i)
{
    struct pair_desc p;
    unsigned int s = 0x9e3779b9u ^ (unsigned int)i * 2654435761u;
    rng(&s);
    p.heavy = (i % 8) == 3;
    p.skinned = rng(&s) % 4 == 0;
    p.tangent = p.skinned || rng(&s) % 2;
    p.extra = rng(&s) % 4;                   /* extra interpolants */
    p.ntex = 1 + rng(&s) % NTEX;             /* textures sampled */
    p.nlights = 1 + rng(&s) % (p.heavy ? 8 : 4);
    p.parallax = p.tangent && rng(&s) % 3 == 0;
    p.fog = rng(&s) % 2;
    if (p.heavy) { p.ntex = NTEX; p.tangent = 1; }
    return p;
}

#define CAT(...) (n += snprintf(b + n, cap - n, __VA_ARGS__))

static const char common_cb[] =
    "cbuffer Obj : register(b0) { float4 rect; float4 color; float4 kz; float4 tint; float4x4 world; float4x4 viewproj; };\n"
    "cbuffer Mat : register(b1) { float4 lightpos[8]; float4 lightcol[8]; float4 params; float4 fogp; float4 eye; };\n";

static void gen_io(char *b, size_t cap, int *pn, const struct pair_desc *p)
{
    int n = *pn;
    CAT("struct VSIn { float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0;");
    if (p->tangent) CAT(" float4 tan : TANGENT;");
    if (p->skinned) CAT(" uint4 bi : BLENDINDICES; float4 bw : BLENDWEIGHT;");
    CAT(" };\n");
    CAT("struct V2P { float4 pos : SV_Position; float2 uv : TEXCOORD0; float3 nrm : NORMAL; float3 wpos : TEXCOORD1;");
    if (p->tangent) CAT(" float4 tan : TANGENT;");
    for (int e = 0; e < p->extra; e++) CAT(" float4 x%d : TEXCOORD%d;", e, e + 2);
    CAT(" };\n");
    *pn = n;
}

static void gen_vs(char *b, size_t cap, int i, const struct pair_desc *p)
{
    int n = 0;
    CAT("%s", common_cb);
    if (p->skinned) CAT("cbuffer Bones : register(b2) { float4x3 bones[48]; };\n");
    gen_io(b, cap, &n, p);
    CAT("V2P main(VSIn v)\n{\n    V2P o;\n    float3 p = v.pos;\n    float3 nn = v.nrm;\n");
    if (p->skinned)
    {
        CAT("    float3 sp = 0, sn = 0;\n");
        CAT("    [unroll] for (int j = 0; j < 4; j++) { sp += mul(float4(p, 1), bones[v.bi[j]]) * v.bw[j];"
            " sn += mul(nn, (float3x3)bones[v.bi[j]]) * v.bw[j]; }\n");
        CAT("    p = lerp(p, sp, kz.x); nn = lerp(nn, sn, kz.x);\n");
    }
    CAT("    float4 wp = mul(float4(p, 1), world);\n");
    CAT("    float4 cp = mul(wp, viewproj);\n");
    if (i % 3 == 0) CAT("    wp.xyz += sin(wp.yzx * %.3f + kz.w) * %.3f;\n", 0.7 + (i % 11) * 0.1, 0.01 + (i % 5) * 0.01);
    CAT("    o.pos = float4(rect.xy + v.pos.xy * rect.zw, 0.5, 1) + cp * kz.y;\n");
    CAT("    o.uv = v.uv * %.3f + %.3f;\n", 1.0 + (i % 7) * 0.25, (i % 13) * 0.05);
    CAT("    o.nrm = normalize(mul(nn, (float3x3)world) + float3(0, 0, 1e-3));\n");
    CAT("    o.wpos = wp.xyz;\n");
    if (p->tangent) CAT("    o.tan = float4(normalize(mul(v.tan.xyz, (float3x3)world) + float3(1e-3, 0, 0)), v.tan.w);\n");
    for (int e = 0; e < p->extra; e++)
        CAT("    o.x%d = float4(v.uv * %.2f, sin(wp.x * %.2f), cos(wp.y * %.2f + %d));\n", e, 1.5 + e, 0.3 * (e + 1), 0.2 * (e + 2), i % 17);
    CAT("    return o;\n}\n");
}

static void gen_ps(char *b, size_t cap, int i, const struct pair_desc *p)
{
    int n = 0;
    CAT("%s", common_cb);
    for (int t = 0; t < p->ntex; t++) CAT("Texture2D t%d : register(t%d);\n", t, t);
    CAT("SamplerState s0 : register(s0);\nSamplerState s1 : register(s1);\n");
    gen_io(b, cap, &n, p);
    CAT("float4 main(V2P i) : SV_Target\n{\n");
    CAT("    float2 uv = i.uv;\n");
    CAT("    float3 v = normalize(eye.xyz - i.wpos + float3(0, 0, 1));\n");
    if (p->parallax)
    {
        CAT("    float3 vt = float3(dot(v, i.tan.xyz), dot(v, cross(i.nrm, i.tan.xyz) * i.tan.w), dot(v, i.nrm));\n");
        CAT("    float h = 1; float2 duv = vt.xy / max(vt.z, 0.1) * params.z / %d;\n", 8 + i % 8);
        CAT("    [loop] for (int k = 0; k < %d; k++) { float s = t0.SampleLevel(s0, uv, 0).a; if (s >= h) break; h -= 1.0 / %d; uv -= duv; }\n",
            8 + i % 8, 8 + i % 8);
    }
    CAT("    float4 albedo = t0.Sample(s0, uv) * tint;\n");
    CAT("    float3 n = normalize(i.nrm);\n");
    if (p->tangent && p->ntex > 1)
    {
        CAT("    float3 tn = t1.Sample(s1, uv * %.2f).xyz * 2 - 1;\n", 1.0 + (i % 3) * 0.5);
        CAT("    float3 bt = cross(n, i.tan.xyz) * i.tan.w;\n");
        CAT("    n = normalize(tn.x * i.tan.xyz + tn.y * bt + tn.z * n);\n");
    }
    CAT("    float rough = 0.5, ao = 1, metal = 0; float3 emis = 0;\n");
    if (p->ntex > 2) CAT("    float4 rm = t2.Sample(s0, uv); rough = rm.g; metal = rm.b; ao = rm.r;\n");
    if (p->ntex > 3) CAT("    emis = t3.Sample(s1, uv * 0.5).rgb * params.w;\n");
    if (p->ntex > 4) CAT("    albedo.rgb = lerp(albedo.rgb, t4.Sample(s0, uv * %.1f).rgb, 0.3);\n", 4.0 + i % 5);
    if (p->ntex > 5) CAT("    ao *= t5.SampleLevel(s1, i.wpos.xy * 0.01, 2).r;\n");
    CAT("    float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo.rgb, metal);\n");
    CAT("    float3 lit = float3(0.03, 0.03, 0.035) * ao;\n");
    for (int l = 0; l < p->nlights; l++)
    {
        CAT("    {\n        float3 l = lightpos[%d].xyz - i.wpos; float d = length(l) + 1e-3; l /= d;\n", l);
        CAT("        float3 hv = normalize(l + v); float ndl = saturate(dot(n, l)); float ndh = saturate(dot(n, hv));\n");
        CAT("        float a = rough * rough; float dd = ndh * ndh * (a * a - 1) + 1; float D = a * a / (3.14159 * dd * dd + 1e-4);\n");
        CAT("        float3 F = f0 + (1 - f0) * pow(1 - saturate(dot(hv, v)), 5);\n");
        CAT("        lit += lightcol[%d].rgb * ndl * (albedo.rgb * (1 - metal) / 3.14159 + D * F * 0.25) / (1 + d * d * lightpos[%d].w);\n", l, l);
        CAT("    }\n");
    }
    for (int e = 0; e < (p->extra < 2 ? p->extra : 2); e++) CAT("    lit += i.x%d.xyz * %.3f;\n", e, 0.001 * (e + 1));
    CAT("    float3 c = albedo.rgb * lit + emis;\n");
    if (p->fog) CAT("    c = lerp(c, fogp.rgb, saturate(length(i.wpos - eye.xyz) * fogp.w));\n");
    if (i % 5 == 0) CAT("    if (params.y > 0.5) c = c / (c + 1); else c = sqrt(c);\n");
    CAT("    return float4(lerp(color.rgb, c * %.3f, kz.z), color.a);\n}\n", 0.9 + (i % 9) * 0.02);
}

/* ---- resources ---- */

struct obj_cb
{
    float rect[4], color[4], kz[4], tint[4], world[16], viewproj[16];
};
struct mat_cb
{
    float lightpos[8][4], lightcol[8][4], params[4], fogp[4], eye[4];
};

struct pair
{
    struct pair_desc d;
    ID3DBlob *vs_code, *ps_code;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11InputLayout *layout;
};

static struct
{
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    ID3D11Texture2D *rt8, *rt16;
    ID3D11RenderTargetView *rtv8, *rtv16;
    ID3D11DepthStencilView *dsv;
    ID3D11DepthStencilState *dss;
    ID3D11RasterizerState *rs;
    ID3D11BlendState *blend[NBLEND];
    ID3D11Buffer *vb, *ib, *cb_obj, *cb_mat, *cb_bones;
    ID3D11ShaderResourceView *srv[NTEX];
    ID3D11SamplerState *smp[2];
    struct pair *pairs;
    int npairs;
} R;

static ID3DBlob *compile(const char *src, const char *target, int i)
{
    ID3DBlob *code = NULL, *errors = NULL;
    char name[32];
    snprintf(name, sizeof(name), "pair%d", i);
    if (FAILED(D3DCompile(src, strlen(src), name, NULL, NULL, "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)))
    {
        fprintf(stderr, "pair %d %s:\n%s\n%s\n", i, target, errors ? (const char *)ID3D10Blob_GetBufferPointer(errors) : "", src);
        return NULL;
    }
    return code;
}

#define BLOB(b) ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b)

static HRESULT create_pair(struct pair *p)
{
    HRESULT hr;
    D3D11_INPUT_ELEMENT_DESC ie[6];
    UINT ne = 0;
    ie[ne++] = (D3D11_INPUT_ELEMENT_DESC){ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
    ie[ne++] = (D3D11_INPUT_ELEMENT_DESC){ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 };
    ie[ne++] = (D3D11_INPUT_ELEMENT_DESC){ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 };
    if (p->d.tangent)
        ie[ne++] = (D3D11_INPUT_ELEMENT_DESC){ "TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 };
    if (p->d.skinned)
    {
        ie[ne++] = (D3D11_INPUT_ELEMENT_DESC){ "BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0 };
        ie[ne++] = (D3D11_INPUT_ELEMENT_DESC){ "BLENDWEIGHT", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 52, D3D11_INPUT_PER_VERTEX_DATA, 0 };
    }
    if (FAILED(hr = ID3D11Device_CreateVertexShader(R.dev, BLOB(p->vs_code), NULL, &p->vs))) return hr;
    if (FAILED(hr = ID3D11Device_CreatePixelShader(R.dev, BLOB(p->ps_code), NULL, &p->ps))) return hr;
    return ID3D11Device_CreateInputLayout(R.dev, ie, ne, BLOB(p->vs_code), &p->layout);
}

static void draw_pair(int i, int variant, int frame)
{
    ID3D11DeviceContext *ctx = R.ctx;
    struct pair *p = &R.pairs[i];
    const UINT stride = STRIDE, offset = 0;
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)R.cb_obj, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
    {
        struct obj_cb *c = m.pData;
        memset(c, 0, sizeof(*c));
        int slot = i * 5 + variant;
        c->rect[0] = (float)(slot % 40) / 20.0f - 1.0f;
        c->rect[1] = (float)((slot / 40) % 50) / 25.0f - 1.0f;
        c->rect[2] = 0.045f;
        c->rect[3] = 0.035f;
        c->color[0] = (float)(i & 255) / 255.0f;
        c->color[1] = (float)((i * 7 + variant * 50) & 255) / 255.0f;
        c->color[2] = (float)((frame / 60) & 7) / 7.0f;
        c->color[3] = 0.8f;
        c->kz[2] = 0.25f; /* shading result shows in the image, the transforms are multiplied by 0 */
        c->tint[0] = c->tint[1] = c->tint[2] = c->tint[3] = 1;
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)R.cb_obj, 0);
    }
    ID3D11DeviceContext_IASetInputLayout(ctx, p->layout);
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &R.vb, &stride, &offset);
    ID3D11DeviceContext_VSSetShader(ctx, p->vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, p->ps, NULL, 0);
    ID3D11DeviceContext_OMSetBlendState(ctx, R.blend[variant % NBLEND], NULL, 0xffffffff);
    ID3D11DeviceContext_DrawIndexed(ctx, 6, 0, 0);
}

static void bind_common(void)
{
    ID3D11DeviceContext *ctx = R.ctx;
    ID3D11Buffer *cbs[3] = { R.cb_obj, R.cb_mat, R.cb_bones };
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 3, cbs);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 2, cbs);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, NTEX, R.srv);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 2, R.smp);
    ID3D11DeviceContext_IASetIndexBuffer(ctx, R.ib, DXGI_FORMAT_R16_UINT, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_RSSetState(ctx, R.rs);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, R.dss, 0);
}

static unsigned int hash_rt(ID3D11Texture2D *rt, int bpp)
{
    D3D11_TEXTURE2D_DESC sd;
    ID3D11Texture2D_GetDesc(rt, &sd);
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *st;
    D3D11_MAPPED_SUBRESOURCE m;
    unsigned int h = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(R.dev, &sd, NULL, &st))) return 0;
    ID3D11DeviceContext_CopyResource(R.ctx, (ID3D11Resource *)st, (ID3D11Resource *)rt);
    if (SUCCEEDED(ID3D11DeviceContext_Map(R.ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m)))
    {
        h = 2166136261u;
        for (int y = 0; y < HEIGHT; y++)
        {
            const unsigned char *row = (const unsigned char *)m.pData + (size_t)y * m.RowPitch;
            for (int x = 0; x < WIDTH * bpp; x++) h = (h ^ row[x]) * 16777619u;
        }
        ID3D11DeviceContext_Unmap(R.ctx, (ID3D11Resource *)st, 0);
    }
    ID3D11Texture2D_Release(st);
    return h;
}

/* pairs that come into use in frame f (measured frames count from 0) */
static int new_in_frame(int f)
{
    if (f < 0) return 0;
    if (f % 60 == 0) return 24;
    return f % 4 == 0;
}

int main(int argc, char **argv)
{
    g_mode = argc > 1 ? argv[1] : "load";
    int npairs = argc > 2 ? atoi(argv[2]) : 400;
    int frames = argc > 3 ? atoi(argv[3]) : 720;
    double fps = argc > 4 ? atof(argv[4]) : 60;
    int stream = !strcmp(g_mode, "stream");
    HRESULT hr;
    if (!stream && strcmp(g_mode, "load")) return fail("unknown mode", E_INVALIDARG);
    if (npairs < INITIAL) npairs = INITIAL;

    /* HLSL to DXBC first, not timed */
    double tc0 = now_ms();
    R.pairs = calloc(npairs, sizeof(*R.pairs));
    R.npairs = npairs;
    static char src[32768];
    size_t vs_bytes = 0, ps_bytes = 0;
    for (int i = 0; i < npairs; i++)
    {
        struct pair *p = &R.pairs[i];
        p->d = describe(i);
        gen_vs(src, sizeof(src), i, &p->d);
        if (!(p->vs_code = compile(src, "vs_5_0", i))) return fail("D3DCompile vs", E_FAIL);
        gen_ps(src, sizeof(src), i, &p->d);
        if (!(p->ps_code = compile(src, "ps_5_0", i))) return fail("D3DCompile ps", E_FAIL);
        vs_bytes += ID3D10Blob_GetBufferSize(p->vs_code);
        ps_bytes += ID3D10Blob_GetBufferSize(p->ps_code);
    }
    double hlsl_ms = now_ms() - tc0;

    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &fl, 1, D3D11_SDK_VERSION, &R.dev, NULL,
                                      &R.ctx)))
        return fail("D3D11CreateDevice", hr);

    D3D11_TEXTURE2D_DESC rtd = { WIDTH, HEIGHT, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT,
                                 D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    if (FAILED(hr = ID3D11Device_CreateTexture2D(R.dev, &rtd, NULL, &R.rt8))) return fail("CreateTexture2D rt8", hr);
    if (FAILED(hr = ID3D11Device_CreateRenderTargetView(R.dev, (ID3D11Resource *)R.rt8, NULL, &R.rtv8))) return fail("RTV8", hr);
    rtd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (FAILED(hr = ID3D11Device_CreateTexture2D(R.dev, &rtd, NULL, &R.rt16))) return fail("CreateTexture2D rt16", hr);
    if (FAILED(hr = ID3D11Device_CreateRenderTargetView(R.dev, (ID3D11Resource *)R.rt16, NULL, &R.rtv16))) return fail("RTV16", hr);
    D3D11_TEXTURE2D_DESC dd = { WIDTH, HEIGHT, 1, 1, DXGI_FORMAT_D24_UNORM_S8_UINT, { 1, 0 }, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_DEPTH_STENCIL, 0, 0 };
    ID3D11Texture2D *depth;
    if (FAILED(hr = ID3D11Device_CreateTexture2D(R.dev, &dd, NULL, &depth))) return fail("CreateTexture2D depth", hr);
    if (FAILED(hr = ID3D11Device_CreateDepthStencilView(R.dev, (ID3D11Resource *)depth, NULL, &R.dsv))) return fail("DSV", hr);

    /* one quad with every vertex attribute any pair reads (stride 56) */
    struct vtx { float pos[3], nrm[3], uv[2], tan[4]; unsigned char bi[4], bw[4]; } quad[4];
    memset(quad, 0, sizeof(quad));
    for (int v = 0; v < 4; v++)
    {
        quad[v].pos[0] = (float)(v & 1); quad[v].pos[1] = (float)(v >> 1); quad[v].pos[2] = 0;
        quad[v].nrm[2] = 1; quad[v].uv[0] = (float)(v & 1); quad[v].uv[1] = (float)(v >> 1);
        quad[v].tan[0] = 1; quad[v].tan[3] = 1;
        quad[v].bi[0] = 0; quad[v].bi[1] = 1; quad[v].bi[2] = 2; quad[v].bi[3] = 3;
        quad[v].bw[0] = 255;
    }
    static const unsigned short idx[6] = { 0, 1, 2, 2, 1, 3 };
    D3D11_BUFFER_DESC bd = { sizeof(quad), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA init = { quad, 0, 0 };
    if (FAILED(hr = ID3D11Device_CreateBuffer(R.dev, &bd, &init, &R.vb))) return fail("CreateBuffer vb", hr);
    D3D11_BUFFER_DESC ibd = { sizeof(idx), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA iinit = { idx, 0, 0 };
    if (FAILED(hr = ID3D11Device_CreateBuffer(R.dev, &ibd, &iinit, &R.ib))) return fail("CreateBuffer ib", hr);
    D3D11_BUFFER_DESC cbd = { sizeof(struct obj_cb), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
    if (FAILED(hr = ID3D11Device_CreateBuffer(R.dev, &cbd, NULL, &R.cb_obj))) return fail("CreateBuffer cb", hr);
    struct mat_cb mat;
    memset(&mat, 0, sizeof(mat));
    for (int l = 0; l < 8; l++)
    {
        mat.lightpos[l][0] = (float)(l % 3) - 1; mat.lightpos[l][1] = (float)(l / 3) - 1; mat.lightpos[l][2] = 2;
        mat.lightpos[l][3] = 0.1f;
        mat.lightcol[l][0] = 0.5f + 0.05f * l; mat.lightcol[l][1] = 0.6f; mat.lightcol[l][2] = 0.7f - 0.05f * l;
    }
    mat.params[0] = 16; mat.params[1] = 0; mat.params[2] = 0.05f; mat.params[3] = 0.5f;
    mat.fogp[0] = 0.5f; mat.fogp[1] = 0.6f; mat.fogp[2] = 0.7f; mat.fogp[3] = 0.05f;
    mat.eye[2] = 3;
    D3D11_SUBRESOURCE_DATA minit = { &mat, 0, 0 };
    cbd.ByteWidth = sizeof(mat); cbd.Usage = D3D11_USAGE_IMMUTABLE; cbd.CPUAccessFlags = 0;
    if (FAILED(hr = ID3D11Device_CreateBuffer(R.dev, &cbd, &minit, &R.cb_mat))) return fail("CreateBuffer mat", hr);
    static float bones[48 * 12];
    for (int k = 0; k < 48; k++) { bones[k * 12 + 0] = 1; bones[k * 12 + 5] = 1; bones[k * 12 + 10] = 1; }
    D3D11_SUBRESOURCE_DATA binit = { bones, 0, 0 };
    cbd.ByteWidth = sizeof(bones);
    if (FAILED(hr = ID3D11Device_CreateBuffer(R.dev, &cbd, &binit, &R.cb_bones))) return fail("CreateBuffer bones", hr);

    for (int t = 0; t < NTEX; t++)
    {
        static unsigned int pixels[64 * 64];
        for (int i = 0; i < 64 * 64; i++)
            pixels[i] = ((i ^ (i >> 6)) & 8) ? 0xffc0c0c0u - t * 0x101010u : 0x80304000u + t * 0x20u + (i & 0x3f) * 0x10000u;
        D3D11_TEXTURE2D_DESC td = { 64, 64, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_IMMUTABLE,
                                    D3D11_BIND_SHADER_RESOURCE, 0, 0 };
        D3D11_SUBRESOURCE_DATA td_init = { pixels, 64 * 4, 0 };
        ID3D11Texture2D *tex;
        if (FAILED(hr = ID3D11Device_CreateTexture2D(R.dev, &td, &td_init, &tex))) return fail("CreateTexture2D", hr);
        if (FAILED(hr = ID3D11Device_CreateShaderResourceView(R.dev, (ID3D11Resource *)tex, NULL, &R.srv[t])))
            return fail("CreateShaderResourceView", hr);
    }
    for (int i = 0; i < 2; i++)
    {
        D3D11_SAMPLER_DESC smd = { i ? D3D11_FILTER_MIN_MAG_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_LINEAR,
                                   D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_WRAP,
                                   0, 1, D3D11_COMPARISON_NEVER, { 0 }, 0, D3D11_FLOAT32_MAX };
        if (FAILED(hr = ID3D11Device_CreateSamplerState(R.dev, &smd, &R.smp[i]))) return fail("CreateSamplerState", hr);
    }
    for (int i = 0; i < NBLEND; i++)
    {
        D3D11_BLEND_DESC bld = { 0 };
        D3D11_RENDER_TARGET_BLEND_DESC *t = &bld.RenderTarget[0];
        t->BlendEnable = i != 0;
        t->SrcBlend = i == 3 ? D3D11_BLEND_ONE : D3D11_BLEND_SRC_ALPHA;
        t->DestBlend = i == 2 ? D3D11_BLEND_ONE : D3D11_BLEND_INV_SRC_ALPHA;
        t->BlendOp = D3D11_BLEND_OP_ADD;
        t->SrcBlendAlpha = D3D11_BLEND_ONE;
        t->DestBlendAlpha = D3D11_BLEND_ZERO;
        t->BlendOpAlpha = D3D11_BLEND_OP_ADD;
        t->RenderTargetWriteMask = i == 3 ? (D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE)
                                          : D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(hr = ID3D11Device_CreateBlendState(R.dev, &bld, &R.blend[i]))) return fail("CreateBlendState", hr);
    }
    D3D11_RASTERIZER_DESC rsd = { D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0, 0, TRUE, FALSE, FALSE, FALSE };
    if (FAILED(hr = ID3D11Device_CreateRasterizerState(R.dev, &rsd, &R.rs))) return fail("CreateRasterizerState", hr);
    D3D11_DEPTH_STENCIL_DESC dsd = { 0 };
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (FAILED(hr = ID3D11Device_CreateDepthStencilState(R.dev, &dsd, &R.dss))) return fail("CreateDepthStencilState", hr);
    D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
    ID3D11Query *q;
    if (FAILED(hr = ID3D11Device_CreateQuery(R.dev, &qd, &q))) return fail("CreateQuery", hr);

    double create_ms = 0;
    if (!stream)
    {
        double t0 = now_ms();
        for (int i = 0; i < npairs; i++)
            if (FAILED(hr = create_pair(&R.pairs[i]))) return fail("create pair", hr);
        create_ms = now_ms() - t0;
    }

    double *frame_ms = calloc(frames, sizeof(double)), *submit_ms = calloc(frames, sizeof(double));
    int *new_pairs = calloc(frames, sizeof(int));
    double budget = fps > 0 ? 1000.0 / fps : 0;
    int active = 0, draws = 0;
    double next = now_ms();
    double cpu_start = 0, t_start = 0;
    for (int f = -WARMUP; f < frames; f++)
    {
        if (f == 0) { cpu_start = proc_cpu_ms(); t_start = now_ms(); }
        double t0 = now_ms();
        int want = f < 0 ? INITIAL : active + new_in_frame(f);
        if (want > npairs) want = npairs;
        int added = want - active;
        if (stream)
            for (int i = active; i < want; i++)
                if (FAILED(hr = create_pair(&R.pairs[i]))) return fail("create pair", hr);
        active = want;

        static const float clear8[4] = { 0.05f, 0.05f, 0.08f, 1 }, clear16[4] = { 0, 0, 0, 0 };
        bind_common();
        ID3D11DeviceContext_OMSetRenderTargets(R.ctx, 1, &R.rtv8, R.dsv);
        ID3D11DeviceContext_ClearRenderTargetView(R.ctx, R.rtv8, clear8);
        ID3D11DeviceContext_ClearDepthStencilView(R.ctx, R.dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1, 0);
        D3D11_VIEWPORT vp = { 0, 0, WIDTH, HEIGHT, 0, 1 };
        ID3D11DeviceContext_RSSetViewports(R.ctx, 1, &vp);
        draws = 0;
        for (int i = 0; i < active; i++)
        {
            int nv = R.pairs[i].d.heavy ? NBLEND : 1;
            for (int v = 0; v < nv; v++) { draw_pair(i, v, f); draws++; }
        }
        /* pass 2: heavy materials again into an HDR target without depth */
        ID3D11DeviceContext_OMSetRenderTargets(R.ctx, 1, &R.rtv16, NULL);
        ID3D11DeviceContext_ClearRenderTargetView(R.ctx, R.rtv16, clear16);
        for (int i = 0; i < active; i++)
            if (R.pairs[i].d.heavy) { draw_pair(i, 4, f); draws++; }
        double t1 = now_ms();
        ID3D11DeviceContext_End(R.ctx, (ID3D11Asynchronous *)q);
        ID3D11DeviceContext_Flush(R.ctx);
        while (ID3D11DeviceContext_GetData(R.ctx, (ID3D11Asynchronous *)q, NULL, 0, 0) == S_FALSE) Sleep(0);
        double t2 = now_ms();
        if (f >= 0)
        {
            frame_ms[f] = t2 - t0;
            submit_ms[f] = t1 - t0;
            new_pairs[f] = added;
        }
        if (budget > 0)
        {
            next += budget;
            double now = now_ms();
            if (now >= next) next = now; /* missed: the next frame starts right away */
            else
            {
                if (next - now > 1.5) Sleep((DWORD)(next - now - 1.2));
                while (now_ms() < next) Sleep(0);
            }
        }
    }
    double wall = now_ms() - t_start, cpu = proc_cpu_ms() - cpu_start;

    double excess = 0, worst_submit = 0;
    int over = 0;
    for (int f = 0; f < frames; f++)
    {
        if (budget > 0 && frame_ms[f] > budget) { over++; excess += frame_ms[f] - budget; }
        if (submit_ms[f] > worst_submit) worst_submit = submit_ms[f];
    }
    const char *csv = getenv("SHADER_STUTTER_CSV");
    if (csv && *csv)
    {
        FILE *fp = fopen(csv, "w");
        if (fp)
        {
            fprintf(fp, "frame,frame_ms,submit_ms,new_pairs\n");
            for (int f = 0; f < frames; f++) fprintf(fp, "%d,%.3f,%.3f,%d\n", f, frame_ms[f], submit_ms[f], new_pairs[f]);
            fclose(fp);
        }
    }
    double *sorted = malloc(frames * sizeof(double));
    memcpy(sorted, frame_ms, frames * sizeof(double));
    qsort(sorted, frames, sizeof(double), cmp_double);
    unsigned int h8 = hash_rt(R.rt8, 4), h16 = hash_rt(R.rt16, 8);

    printf("{\"bench\":\"shader_stutter\",\"mode\":\"%s\",\"pairs\":%d,\"active\":%d,\"frames\":%d,\"fps_target\":%.0f,"
           "\"hlsl_ms\":%.0f,\"dxbc_kb\":%.0f,\"create_ms\":%.1f,\"frame_ms_med\":%.3f,\"frame_ms_p99\":%.2f,"
           "\"frame_ms_max\":%.2f,\"submit_ms_max\":%.2f,\"over_budget\":%d,\"excess_ms\":%.1f,\"wall_s\":%.2f,"
           "\"proc_cpu_ms\":%.0f,\"cpu_ms_per_frame\":%.3f,\"draws_last\":%d,\"hash_rgba8\":\"%08x\",\"hash_rgba16f\":\"%08x\"}\n",
           g_mode, npairs, active, frames, fps, hlsl_ms, (vs_bytes + ps_bytes) / 1024.0, create_ms, sorted[frames / 2],
           sorted[(int)(frames * 0.99)], sorted[frames - 1], worst_submit, over, excess, wall / 1000.0,
           cpu, cpu / frames, draws, h8, h16);
    fflush(stdout);
    return 0;
}
