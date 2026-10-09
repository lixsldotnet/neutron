/* neutron bench: D3D11 GPU time per frame of an engine-like frame, headless.
 *
 * x86_64 Windows program. Renders a deferred frame at 2924x1224 (neutron's default
 * internal resolution) into offscreen targets, no window, no swapchain:
 *
 *   shadow    4 cascades, 2048x2048 R32_TYPELESS array, depth only, each slice cleared
 *   gbuffer   4 MRTs (RGBA8 sRGB, RGB10A2, RGBA8, RGBA16F) + D32S8 depth, cleared
 *   ssao      full-screen, 16 depth taps into R8 (read by lighting)
 *   lighting  full-screen pass reading the G-buffer, depth, AO and shadows (PCF) into HDR RGBA16F
 *   exposure  compute pass, luminance per 64x64 tile into a small UAV texture
 *   translucent  blended quads on HDR with a read-only depth-stencil view
 *   msaa      (opt) forward pass into 4x RGBA16F + 4x D32, ResolveSubresource
 *   taa       HDR + history (ping-pong RGBA16F) with neighborhood clamp
 *   bloom     6 downsample passes (1/2 .. 1/64) and 5 additive upsample passes
 *   tonemap   TAA + bloom + exposure (+ msaa) into an RGBA8 sRGB target, UI quads on top
 *
 * GPU time comes from TIMESTAMP queries at frame start and end (DXMT samples Metal GPU
 * timestamps), wall time per frame from QueryPerformanceCounter with at most 3 frames in
 * flight (GPU-bound, so close to the GPU time). After the run the final LDR target, the
 * HDR target and the depth buffer are read back and hashed (FNV-1a), so two DXMT builds
 * can be checked for the same picture. dump=<prefix> also writes the raw readbacks.
 *
 * Options (any order):
 *   <frames>     measured frames (default 300, plus 30 warmup)
 *   noclear      skip the per-frame ClearRenderTargetView on G-buffer, HDR and bloom
 *   discard      DiscardView on bloom targets before use, G-buffer and depth after the
 *                last read, MSAA targets after the resolve (what Unity and others do)
 *   msaa         add the 4x MSAA forward pass and its resolve
 *   update       per draw constants with UpdateSubresource on a DEFAULT buffer (UE4/5
 *                D3D11 RHI style) instead of Map(WRITE_DISCARD)
 *   nots         no timestamp queries (wall time only)
 *   bgra         8 bit targets as B8G8R8A8_TYPELESS (Unreal) instead of R8G8B8A8_TYPELESS (Unity)
 *   lights=<n>   point lights in the lighting pass (default 32)
 *   poll         GetData (flags 0) on the previous frame's event query between the G-buffer
 *                clears and its draws, like a game polling its frame fence mid-recording
 *   occl         an occlusion query Begin/End between the G-buffer clears and its draws
 *   notaa        skip the TAA pass (bloom and tonemap read the HDR target)
 *   dump=<p>     write <p>.ldr.raw, <p>.hdr.raw, <p>.depth.raw (Windows path, e.g. Z:/tmp/x)
 *
 * Prints one JSON line:
 *   {"bench":"gpu_headless",...,"gpu_ms":..,"gpu_min_ms":..,"gpu_p10_ms":..,"wall_ms":..,"hash_ldr":"..",...}
 * gpu_ms is the median GPU time per frame, gpu_p10_ms its 10th percentile (less hit by other GPU load).
 *
 * Build: x86_64-w64-mingw32-clang -O2 -o gpu_headless.exe gpu_headless.c -ld3d11 -ld3dcompiler
 * Run:   tool/neutron runinprefix gpu_headless.exe [options]
 * A/B:   dev/bench/gpu-ab.sh <runtime-A> <runtime-B> [runs] [frames] [options]
 * With a DXMT that has the dev pass log patch, DXMT_PASS_LOG=100,2 DXMT_LOG_LEVEL=info prints the
 * render passes of two frames with their load/store actions.
 */
#define COBJMACROS
#define INITGUID
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 2924
#define H 1224
#define SHADOW 2048
#define CASCADES 4
#define BLOOM 6
#define WARMUP 30
#define LATENCY 3
#define GRID 32 /* grid patch cells per side */
#define INST_GB 512
#define DRAWS_GB 16
#define TRANS_DRAWS 8

static const char src[] =
    "cbuffer frame : register(b0) { float4 cam; float4 proj; float4 light; float4 time; };\n"
    "cbuffer draw : register(b1) { float4 tint; float4 params; };\n"
    "Texture2D albedo_tex : register(t0);\n"
    "Texture2D normal_tex : register(t1);\n"
    "SamplerState lin : register(s0);\n"
    "SamplerComparisonState cmp : register(s1);\n"
    "struct vin { float3 pos : POSITION; float4 inst : TEXCOORD1; };\n"
    "struct v2p { float4 pos : SV_Position; float3 wpos : TEXCOORD0; float2 uv : TEXCOORD1; float3 n : NORMAL; };\n"
    "float3 world(vin i) {\n"
    "  float3 p = i.pos * i.inst.w + i.inst.xyz;\n"
    "  p.z += 0.15 * sin(p.x * 3.1 + time.x) * cos(p.y * 2.3);\n"
    "  return p;\n"
    "}\n"
    "float4 project(float3 p) {\n"
    "  float3 v = p - cam.xyz;\n"
    "  return float4(v.x * proj.x, v.y * proj.y, v.z * proj.z + proj.w, v.z);\n"
    "}\n"
    "v2p vs_gb(vin i) {\n"
    "  v2p o; float3 p = world(i);\n"
    "  o.pos = project(p); o.wpos = p; o.uv = i.pos.xy * 4.0;\n"
    "  o.n = normalize(float3(-0.45 * cos(p.x * 3.1 + time.x), 0.3 * sin(p.y * 2.3), -1));\n"
    "  return o;\n"
    "}\n"
    "float4 vs_shadow(vin i) : SV_Position {\n"
    "  float3 p = world(i);\n"
    "  float s = light.w;\n"
    "  return float4((p.x - light.x * p.z) * s, (p.y - light.y * p.z) * s, saturate(p.z / 64.0), 1);\n"
    "}\n"
    "struct gbo { float4 a : SV_Target0; float4 n : SV_Target1; float4 m : SV_Target2; float4 e : SV_Target3; };\n"
    "gbo ps_gb(v2p i) {\n"
    "  gbo o;\n"
    "  float4 c = albedo_tex.Sample(lin, i.uv);\n"
    "  float3 nt = normal_tex.Sample(lin, i.uv * 1.7).xyz * 2 - 1;\n"
    "  float3 n = normalize(i.n + nt * 0.5);\n"
    "  float f = frac(sin(dot(floor(i.uv * 8), float2(12.9898, 78.233))) * 43758.5453);\n"
    "  o.a = float4(c.rgb * tint.rgb * (0.6 + 0.4 * f), 1);\n"
    "  o.n = float4(n * 0.5 + 0.5, 1);\n"
    "  o.m = float4(params.x, params.y, f, 1);\n"
    "  o.e = float4(tint.rgb * params.z * f, 0);\n"
    "  return o;\n"
    "}\n"
    /* full-screen triangle */
    "struct fs { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "fs vs_full(uint id : SV_VertexID) {\n"
    "  fs o; float2 t = float2((id << 1) & 2, id & 2);\n"
    "  o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = t; return o;\n"
    "}\n"
    "Texture2D gb0 : register(t0); Texture2D gb1 : register(t1); Texture2D gb2 : register(t2);\n"
    "Texture2D gb3 : register(t3); Texture2D<float> depth : register(t4);\n"
    "Texture2DArray<float> shadow : register(t5);\n"
    "Texture2D<float> ao_tex : register(t6);\n"
    "float4 ps_light(fs i) : SV_Target {\n"
    "  int3 c = int3(i.pos.xy, 0);\n"
    "  float4 a = gb0.Load(c); float3 n = gb1.Load(c).xyz * 2 - 1; float4 m = gb2.Load(c); float4 e = gb3.Load(c);\n"
    "  float d = depth.Load(c);\n"
    "  float z = proj.w / (d - proj.z);\n"
    "  float3 p = cam.xyz + float3((i.uv.x * 2 - 1) * z / proj.x, (1 - i.uv.y * 2) * z / proj.y, z);\n"
    "  uint cas = min(3u, (uint)(z / 12.0));\n"
    "  float s = light.w / (1 << cas);\n"
    "  float2 suv = float2((p.x - light.x * p.z) * s, (p.y - light.y * p.z) * s) * float2(0.5, -0.5) + 0.5;\n"
    "  float sd = saturate(p.z / 64.0) - 0.002;\n"
    "  float sh = 0;\n"
    "  [unroll] for (int k = 0; k < 4; k++) {\n"
    "    float2 o = float2((k & 1) ? 1.5 : -1.5, (k & 2) ? 1.5 : -1.5) / 2048.0;\n"
    "    sh += shadow.SampleCmpLevelZero(cmp, float3(suv + o, cas), sd);\n"
    "  }\n"
    "  sh *= 0.25;\n"
    "  float3 l = normalize(float3(light.xy, -1));\n"
    "  float3 col = a.rgb * (0.08 + sh * saturate(dot(n, -l)) * 3.0);\n"
    "  [loop] for (int j = 0; j < (int)time.y; j++) {\n"
    "    float3 lp = float3(sin(time.x + j * 0.8) * 6, cos(time.x * 0.7 + j) * 3, 8 + j * 3);\n"
    "    float3 dl = lp - p; float dd = dot(dl, dl);\n"
    "    col += a.rgb * saturate(dot(n, dl * rsqrt(dd))) * (4.0 / (1 + dd)) * float3(1, 0.8 - j * 0.05, 0.5 + j * 0.06);\n"
    "  }\n"
    "  col *= ao_tex.Load(c);\n"
    "  col += e.rgb * 4.0 + m.z * 0.02;\n"
    "  return float4(col, 1);\n"
    "}\n"
    "Texture2D hdr_in : register(t0);\n"
    "RWTexture2D<float> lum_out : register(u0);\n"
    "groupshared float acc[64];\n"
    "[numthreads(8, 8, 1)] void cs_lum(uint3 g : SV_GroupID, uint3 t : SV_GroupThreadID, uint gi : SV_GroupIndex) {\n"
    "  float sum = 0;\n"
    "  for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {\n"
    "    int2 c = int2(g.xy * 64 + t.xy * 8 + int2(x, y));\n"
    "    sum += dot(hdr_in.Load(int3(c, 0)).rgb, float3(0.2126, 0.7152, 0.0722));\n"
    "  }\n"
    "  acc[gi] = sum / 64; GroupMemoryBarrierWithGroupSync();\n"
    "  if (gi == 0) { float s = 0; for (int k = 0; k < 64; k++) s += acc[k]; lum_out[g.xy] = s / 64; }\n"
    "}\n"
    "struct tin { float3 pos : POSITION; float4 inst : TEXCOORD1; };\n"
    "v2p vs_trans(tin i) {\n"
    "  v2p o; float3 p = i.pos * i.inst.w + i.inst.xyz;\n"
    "  o.pos = project(p); o.wpos = p; o.uv = i.pos.xy; o.n = float3(0, 0, -1); return o;\n"
    "}\n"
    "float4 ps_trans(v2p i) : SV_Target {\n"
    "  float2 d = frac(i.uv * 2) - 0.5;\n"
    "  return float4(tint.rgb * (1 - dot(d, d) * 3), 0.35);\n"
    "}\n"
    "float4 ps_fwd(v2p i) : SV_Target {\n"
    "  float4 c = albedo_tex.Sample(lin, i.uv);\n"
    "  return float4(c.rgb * tint.rgb * saturate(dot(i.n, float3(0.3, 0.5, -0.8))) * 2, 1);\n"
    "}\n"
    "Texture2D src_tex : register(t0);\n"
    "float4 ps_down(fs i) : SV_Target {\n"
    "  float2 t = params.xy;\n"
    "  float3 c = src_tex.Sample(lin, i.uv + float2(-t.x, -t.y)).rgb + src_tex.Sample(lin, i.uv + float2(t.x, -t.y)).rgb +\n"
    "             src_tex.Sample(lin, i.uv + float2(-t.x, t.y)).rgb + src_tex.Sample(lin, i.uv + float2(t.x, t.y)).rgb;\n"
    "  c = c * 0.25;\n"
    "  if (params.z > 0) c = max(c - params.z, 0);\n"
    "  return float4(c, 1);\n"
    "}\n"
    "float4 ps_up(fs i) : SV_Target {\n"
    "  float2 t = params.xy;\n"
    "  float3 c = src_tex.Sample(lin, i.uv + float2(-t.x, 0)).rgb + src_tex.Sample(lin, i.uv + float2(t.x, 0)).rgb +\n"
    "             src_tex.Sample(lin, i.uv + float2(0, -t.y)).rgb + src_tex.Sample(lin, i.uv + float2(0, t.y)).rgb;\n"
    "  return float4(c * 0.25 * params.w, 1);\n"
    "}\n"
    "Texture2D t_hdr : register(t0); Texture2D t_bloom : register(t1); Texture2D<float> t_lum : register(t2);\n"
    "Texture2D t_msaa : register(t3);\n"
    "float4 ps_tone(fs i) : SV_Target {\n"
    "  float3 c = t_hdr.Load(int3(i.pos.xy, 0)).rgb + t_bloom.Sample(lin, i.uv).rgb * 0.3;\n"
    "  c += t_msaa.Load(int3(i.pos.xy, 0)).rgb * params.x;\n"
    "  float l = t_lum.Load(int3(2, 2, 0)) + 0.18;\n"
    "  c *= 0.5 / l;\n"
    "  c = c / (1 + c);\n"
    "  return float4(c, 1);\n"
    "}\n"
    "float4 ps_ui(fs i) : SV_Target { return float4(tint.rgb, 0.5); }\n"
    "float ps_ssao(fs i) : SV_Target {\n"
    "  int2 c = int2(i.pos.xy);\n"
    "  float d0 = depth.Load(int3(c, 0)); float z0 = proj.w / (d0 - proj.z);\n"
    "  float occ = 0;\n"
    "  [unroll] for (int k = 0; k < 16; k++) {\n"
    "    float a = k * 2.399 + frac(sin(dot(float2(c), float2(12.9898, 78.233))) * 43758.5453) * 6.283;\n"
    "    int2 o = int2(float2(cos(a), sin(a)) * (4 + k * 3));\n"
    "    float d = depth.Load(int3(clamp(c + o, int2(0, 0), int2(2923, 1223)), 0));\n"
    "    float z = proj.w / (d - proj.z);\n"
    "    occ += saturate((z0 - z) * 2.0) * saturate(1.5 - abs(z0 - z));\n"
    "  }\n"
    "  return 1 - occ / 16;\n"
    "}\n"
    "Texture2D taa_cur : register(t0); Texture2D taa_hist : register(t1);\n"
    "float4 ps_taa(fs i) : SV_Target {\n"
    "  int2 c = int2(i.pos.xy);\n"
    "  float3 mn = 1e9, mx = -1e9, cur = 0;\n"
    "  [unroll] for (int y = -1; y <= 1; y++) [unroll] for (int x = -1; x <= 1; x++) {\n"
    "    float3 v = taa_cur.Load(int3(c + int2(x, y), 0)).rgb; mn = min(mn, v); mx = max(mx, v);\n"
    "    if (x == 0 && y == 0) cur = v;\n"
    "  }\n"
    "  float3 h = clamp(taa_hist.Sample(lin, i.uv + float2(0.0002, 0)).rgb, mn, mx);\n"
    "  return float4(lerp(cur, h, 0.9 * params.x), 1);\n"
    "}\n";

struct frame_cb { float cam[4], proj[4], light[4], time[4]; };
struct draw_cb { float tint[4], params[4]; };

static const char *g_opts = "";

static int fail(const char *what, HRESULT hr)
{
    printf("{\"bench\":\"gpu_headless\",\"opts\":\"%s\",\"error\":\"%s hr=0x%08lx\"}\n", g_opts, what, (unsigned long)hr);
    fflush(stdout);
    exit(1);
}
#define CHK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) fail(#x, hr_); } while (0)

static ID3DBlob *compile(const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *errors = NULL;
    if (FAILED(D3DCompile(src, sizeof(src) - 1, "gpu", NULL, NULL, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code,
                          &errors)))
    {
        if (errors) fprintf(stderr, "%s: %s\n", entry, (const char *)ID3D10Blob_GetBufferPointer(errors));
        fail(entry, E_FAIL);
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

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static double median(double *v, int n)
{
    if (!n) return 0;
    qsort(v, n, sizeof(*v), cmp_double);
    return n & 1 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static ID3D11DeviceContext1 *ctx1;

static ID3D11Texture2D *tex2d(UINT w, UINT h, UINT mips, UINT array, DXGI_FORMAT fmt, UINT bind, UINT samples)
{
    D3D11_TEXTURE2D_DESC d = { w, h, mips, array, fmt, { samples, 0 }, D3D11_USAGE_DEFAULT, bind, 0, 0 };
    ID3D11Texture2D *t;
    CHK(ID3D11Device_CreateTexture2D(dev, &d, NULL, &t));
    return t;
}

static ID3D11RenderTargetView *rtv(ID3D11Texture2D *t, DXGI_FORMAT fmt, int ms)
{
    D3D11_RENDER_TARGET_VIEW_DESC d = { 0 };
    ID3D11RenderTargetView *v;
    d.Format = fmt;
    d.ViewDimension = ms ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
    CHK(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)t, &d, &v));
    return v;
}

static ID3D11ShaderResourceView *srv(ID3D11Texture2D *t, DXGI_FORMAT fmt)
{
    D3D11_SHADER_RESOURCE_VIEW_DESC d = { 0 };
    ID3D11ShaderResourceView *v;
    d.Format = fmt;
    d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    d.Texture2D.MipLevels = -1;
    CHK(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)t, &d, &v));
    return v;
}

static uint64_t fnv(const uint8_t *p, size_t rows, size_t row_bytes, size_t pitch, uint64_t h)
{
    for (size_t y = 0; y < rows; y++)
        for (size_t x = 0; x < row_bytes; x++) h = (h ^ p[y * pitch + x]) * 0x100000001b3ull;
    return h;
}

/* copy a texture to a staging copy, hash it, optionally write it */
static uint64_t readback(ID3D11Texture2D *t, UINT row_bytes, const char *dump, const char *suffix)
{
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D *st;
    D3D11_MAPPED_SUBRESOURCE m;
    uint64_t h = 0xcbf29ce484222325ull;
    ID3D11Texture2D_GetDesc(t, &d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MipLevels = 1;
    d.MiscFlags = 0;
    CHK(ID3D11Device_CreateTexture2D(dev, &d, NULL, &st));
    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)st, 0, 0, 0, 0, (ID3D11Resource *)t, 0, NULL);
    CHK(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m));
    h = fnv(m.pData, d.Height, row_bytes, m.RowPitch, h);
    if (dump)
    {
        char path[MAX_PATH];
        FILE *f;
        snprintf(path, sizeof(path), "%s.%s.raw", dump, suffix);
        if ((f = fopen(path, "wb")))
        {
            for (UINT y = 0; y < d.Height; y++) fwrite((uint8_t *)m.pData + (size_t)y * m.RowPitch, 1, row_bytes, f);
            fclose(f);
        }
    }
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
    ID3D11Texture2D_Release(st);
    return h;
}

int main(int argc, char **argv)
{
    int frames = 300, noclear = 0, discard = 0, msaa = 0, update = 0, ts = 1, lights = 32, bgra = 0, poll = 0, occl = 0, notaa = 0;
    const char *dump = NULL;
    static char optbuf[256];
    for (int i = 1; i < argc; i++)
    {
        if (atoi(argv[i]) > 0) frames = atoi(argv[i]);
        else if (!strcmp(argv[i], "noclear")) noclear = 1;
        else if (!strcmp(argv[i], "discard")) discard = 1;
        else if (!strcmp(argv[i], "msaa")) msaa = 1;
        else if (!strcmp(argv[i], "update")) update = 1;
        else if (!strcmp(argv[i], "nots")) ts = 0;
        else if (!strcmp(argv[i], "bgra")) bgra = 1;
        else if (!strcmp(argv[i], "poll")) poll = 1;
        else if (!strcmp(argv[i], "occl")) occl = 1;
        else if (!strcmp(argv[i], "notaa")) notaa = 1;
        else if (!strncmp(argv[i], "lights=", 7)) lights = atoi(argv[i] + 7);
        else if (!strncmp(argv[i], "dump=", 5)) dump = argv[i] + 5;
        if (strncmp(argv[i], "dump=", 5) && atoi(argv[i]) <= 0)
        {
            strncat(optbuf, optbuf[0] ? "," : "", sizeof(optbuf) - strlen(optbuf) - 1);
            strncat(optbuf, argv[i], sizeof(optbuf) - strlen(optbuf) - 1);
        }
    }
    g_opts = optbuf;

    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
    CHK(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &fl, 1, D3D11_SDK_VERSION, &dev, NULL, &ctx));
    CHK(ID3D11DeviceContext_QueryInterface(ctx, &IID_ID3D11DeviceContext1, (void **)&ctx1));

    /* --- shaders --- */
    ID3DBlob *b_vs_gb = compile("vs_gb", "vs_5_0"), *b;
    ID3D11VertexShader *vs_gb, *vs_shadow, *vs_full, *vs_trans;
    ID3D11PixelShader *ps_gb, *ps_light, *ps_trans, *ps_fwd, *ps_down, *ps_up, *ps_tone, *ps_ui, *ps_ssao, *ps_taa;
    ID3D11ComputeShader *cs_lum;
    CHK(ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(b_vs_gb), ID3D10Blob_GetBufferSize(b_vs_gb), NULL, &vs_gb));
#define VS(name) b = compile(#name, "vs_5_0"); \
    CHK(ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &name))
#define PS(name) b = compile(#name, "ps_5_0"); \
    CHK(ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &name))
    VS(vs_shadow); VS(vs_full); VS(vs_trans);
    PS(ps_gb); PS(ps_light); PS(ps_trans); PS(ps_fwd); PS(ps_down); PS(ps_up); PS(ps_tone); PS(ps_ui); PS(ps_ssao); PS(ps_taa);
    b = compile("cs_lum", "cs_5_0");
    CHK(ID3D11Device_CreateComputeShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &cs_lum));

    D3D11_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
    };
    ID3D11InputLayout *layout;
    CHK(ID3D11Device_CreateInputLayout(dev, il, 2, ID3D10Blob_GetBufferPointer(b_vs_gb), ID3D10Blob_GetBufferSize(b_vs_gb), &layout));

    /* --- geometry: one grid patch, instances spread over the view --- */
    static float verts[(GRID + 1) * (GRID + 1) * 3];
    static uint16_t idx[GRID * GRID * 6];
    for (int y = 0, n = 0; y <= GRID; y++)
        for (int x = 0; x <= GRID; x++, n++)
        {
            verts[n * 3] = (float)x / GRID - 0.5f;
            verts[n * 3 + 1] = (float)y / GRID - 0.5f;
            verts[n * 3 + 2] = 0;
        }
    for (int y = 0, n = 0; y < GRID; y++)
        for (int x = 0; x < GRID; x++)
        {
            uint16_t a = y * (GRID + 1) + x, c = a + GRID + 1;
            idx[n++] = a; idx[n++] = c; idx[n++] = a + 1;
            idx[n++] = a + 1; idx[n++] = c; idx[n++] = c + 1;
        }
    static float inst[INST_GB * 4], tinst[64 * 4];
    srand(1234);
    for (int i = 0; i < INST_GB; i++)
    {
        float z = 4.0f + (float)(INST_GB - i) * 40.0f / INST_GB; /* far to near: overdraw */
        inst[i * 4] = ((float)rand() / RAND_MAX - 0.5f) * z * 6.0f;
        inst[i * 4 + 1] = ((float)rand() / RAND_MAX - 0.5f) * z * 2.6f;
        inst[i * 4 + 2] = z;
        inst[i * 4 + 3] = z * 0.45f;
    }
    for (int i = 0; i < 64; i++)
    {
        float z = 3.0f + (float)(64 - i) * 0.2f;
        tinst[i * 4] = ((float)rand() / RAND_MAX - 0.5f) * z * 5.0f;
        tinst[i * 4 + 1] = ((float)rand() / RAND_MAX - 0.5f) * z * 2.0f;
        tinst[i * 4 + 2] = z;
        tinst[i * 4 + 3] = z * 0.3f;
    }
    ID3D11Buffer *vb, *ib, *ivb, *tivb, *cb_frame, *cb_draw;
    {
        D3D11_BUFFER_DESC d = { sizeof(verts), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
        D3D11_SUBRESOURCE_DATA s = { verts };
        CHK(ID3D11Device_CreateBuffer(dev, &d, &s, &vb));
        d.ByteWidth = sizeof(idx); d.BindFlags = D3D11_BIND_INDEX_BUFFER; s.pSysMem = idx;
        CHK(ID3D11Device_CreateBuffer(dev, &d, &s, &ib));
        d.ByteWidth = sizeof(inst); d.BindFlags = D3D11_BIND_VERTEX_BUFFER; s.pSysMem = inst;
        CHK(ID3D11Device_CreateBuffer(dev, &d, &s, &ivb));
        d.ByteWidth = sizeof(tinst); s.pSysMem = tinst;
        CHK(ID3D11Device_CreateBuffer(dev, &d, &s, &tivb));
        D3D11_BUFFER_DESC c = { sizeof(struct frame_cb), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
        CHK(ID3D11Device_CreateBuffer(dev, &c, NULL, &cb_frame));
        if (update) { c.Usage = D3D11_USAGE_DEFAULT; c.CPUAccessFlags = 0; }
        c.ByteWidth = sizeof(struct draw_cb);
        CHK(ID3D11Device_CreateBuffer(dev, &c, NULL, &cb_draw));
    }

    /* --- material textures with mips --- */
    ID3D11ShaderResourceView *srv_albedo, *srv_normal;
    {
        static uint32_t px[1024 * 1024];
        D3D11_SUBRESOURCE_DATA s[11];
        for (int k = 0; k < 2; k++)
        {
            for (int y = 0; y < 1024; y++)
                for (int x = 0; x < 1024; x++)
                    px[y * 1024 + x] = k ? (0xff000000u | (128u + ((x * 7) & 63)) | ((128u + ((y * 5) & 63)) << 8) | 0xff0000u)
                                         : (0xff000000u | ((((x ^ y) & 255) * 0x010101u) ^ ((x >> 4) & 1 ? 0x204060u : 0x604020u)));
            for (int l = 0; l < 11; l++) { s[l].pSysMem = px; s[l].SysMemSlicePitch = 0; }
            for (int l = 0; l < 11; l++) s[l].SysMemPitch = 4096; /* each level reads the top left of the base */
            D3D11_TEXTURE2D_DESC d = { 1024, 1024, 11, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
            ID3D11Texture2D *t;
            CHK(ID3D11Device_CreateTexture2D(dev, &d, s, &t));
            CHK(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)t, NULL, k ? &srv_normal : &srv_albedo));
        }
    }

    /* --- targets --- */
    /* bgra: the 8 bit targets as B8G8R8A8_TYPELESS like Unreal's D3D11 RHI, else R8G8B8A8_TYPELESS (Unity) */
    const DXGI_FORMAT t8 = bgra ? DXGI_FORMAT_B8G8R8A8_TYPELESS : DXGI_FORMAT_R8G8B8A8_TYPELESS;
    const DXGI_FORMAT u8 = bgra ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    const DXGI_FORMAT s8 = bgra ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    const DXGI_FORMAT gb_fmt[4] = { t8, DXGI_FORMAT_R10G10B10A2_TYPELESS, t8, DXGI_FORMAT_R16G16B16A16_TYPELESS };
    const DXGI_FORMAT gb_rtv[4] = { s8, DXGI_FORMAT_R10G10B10A2_UNORM, u8, DXGI_FORMAT_R16G16B16A16_FLOAT };
    const DXGI_FORMAT gb_srv[4] = { s8, DXGI_FORMAT_R10G10B10A2_UNORM, u8, DXGI_FORMAT_R16G16B16A16_FLOAT };
    ID3D11Texture2D *gb[4];
    ID3D11RenderTargetView *gb_rt[4];
    ID3D11ShaderResourceView *gb_sr[4];
    for (int i = 0; i < 4; i++)
    {
        gb[i] = tex2d(W, H, 1, 1, gb_fmt[i], D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 1);
        gb_rt[i] = rtv(gb[i], gb_rtv[i], 0);
        gb_sr[i] = srv(gb[i], gb_srv[i]);
    }
    ID3D11Texture2D *depth = tex2d(W, H, 1, 1, DXGI_FORMAT_R32G8X24_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, 1);
    ID3D11DepthStencilView *dsv, *dsv_ro;
    ID3D11ShaderResourceView *depth_sr = srv(depth, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS);
    {
        D3D11_DEPTH_STENCIL_VIEW_DESC d = { DXGI_FORMAT_D32_FLOAT_S8X24_UINT, D3D11_DSV_DIMENSION_TEXTURE2D, 0 };
        CHK(ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)depth, &d, &dsv));
        d.Flags = D3D11_DSV_READ_ONLY_DEPTH | D3D11_DSV_READ_ONLY_STENCIL;
        CHK(ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)depth, &d, &dsv_ro));
    }
    ID3D11Texture2D *shadow = tex2d(SHADOW, SHADOW, 1, CASCADES, DXGI_FORMAT_R32_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, 1);
    ID3D11DepthStencilView *shadow_ds[CASCADES];
    ID3D11ShaderResourceView *shadow_sr;
    for (int i = 0; i < CASCADES; i++)
    {
        D3D11_DEPTH_STENCIL_VIEW_DESC d = { DXGI_FORMAT_D32_FLOAT, D3D11_DSV_DIMENSION_TEXTURE2DARRAY, 0 };
        d.Texture2DArray.FirstArraySlice = i;
        d.Texture2DArray.ArraySize = 1;
        CHK(ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)shadow, &d, &shadow_ds[i]));
    }
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC d = { DXGI_FORMAT_R32_FLOAT, D3D11_SRV_DIMENSION_TEXTURE2DARRAY };
        d.Texture2DArray.MipLevels = 1;
        d.Texture2DArray.ArraySize = CASCADES;
        CHK(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)shadow, &d, &shadow_sr));
    }
    ID3D11Texture2D *hdr = tex2d(W, H, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 1);
    ID3D11RenderTargetView *hdr_rt = rtv(hdr, DXGI_FORMAT_R16G16B16A16_FLOAT, 0);
    ID3D11ShaderResourceView *hdr_sr = srv(hdr, DXGI_FORMAT_R16G16B16A16_FLOAT);
    ID3D11Texture2D *bloom[BLOOM];
    ID3D11RenderTargetView *bloom_rt[BLOOM];
    ID3D11ShaderResourceView *bloom_sr[BLOOM];
    UINT bw[BLOOM], bh[BLOOM];
    for (int i = 0; i < BLOOM; i++)
    {
        bw[i] = (W >> (i + 1)) ? W >> (i + 1) : 1;
        bh[i] = (H >> (i + 1)) ? H >> (i + 1) : 1;
        bloom[i] = tex2d(bw[i], bh[i], 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 1);
        bloom_rt[i] = rtv(bloom[i], DXGI_FORMAT_R16G16B16A16_FLOAT, 0);
        bloom_sr[i] = srv(bloom[i], DXGI_FORMAT_R16G16B16A16_FLOAT);
    }
    UINT lw = (W + 63) / 64, lh = (H + 63) / 64;
    ID3D11Texture2D *lum = tex2d(lw, lh, 1, 1, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, 1);
    ID3D11UnorderedAccessView *lum_uav;
    CHK(ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)lum, NULL, &lum_uav));
    ID3D11ShaderResourceView *lum_sr = srv(lum, DXGI_FORMAT_R32_FLOAT);
    ID3D11Texture2D *ldr = tex2d(W, H, 1, 1, t8, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 1);
    ID3D11RenderTargetView *ldr_rt = rtv(ldr, s8, 0);
    ID3D11Texture2D *ao = tex2d(W, H, 1, 1, DXGI_FORMAT_R8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 1);
    ID3D11RenderTargetView *ao_rt = rtv(ao, DXGI_FORMAT_R8_UNORM, 0);
    ID3D11ShaderResourceView *ao_sr = srv(ao, DXGI_FORMAT_R8_UNORM);
    ID3D11Texture2D *taa[2];
    ID3D11RenderTargetView *taa_rt[2];
    ID3D11ShaderResourceView *taa_sr[2];
    for (int i = 0; i < 2; i++)
    {
        const float z[4] = { 0 };
        taa[i] = tex2d(W, H, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 1);
        taa_rt[i] = rtv(taa[i], DXGI_FORMAT_R16G16B16A16_FLOAT, 0);
        taa_sr[i] = srv(taa[i], DXGI_FORMAT_R16G16B16A16_FLOAT);
        ID3D11DeviceContext_ClearRenderTargetView(ctx, taa_rt[i], z);
    }
    /* msaa forward targets, resolved into fwd */
    ID3D11Texture2D *ms_col = NULL, *ms_dep = NULL, *fwd;
    ID3D11RenderTargetView *ms_rt = NULL;
    ID3D11DepthStencilView *ms_ds = NULL;
    fwd = tex2d(W, H, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 1);
    ID3D11ShaderResourceView *fwd_sr = srv(fwd, DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (msaa)
    {
        ms_col = tex2d(W, H, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET, 4);
        ms_rt = rtv(ms_col, DXGI_FORMAT_R16G16B16A16_FLOAT, 1);
        ms_dep = tex2d(W, H, 1, 1, DXGI_FORMAT_D32_FLOAT, D3D11_BIND_DEPTH_STENCIL, 4);
        D3D11_DEPTH_STENCIL_VIEW_DESC d = { DXGI_FORMAT_D32_FLOAT, D3D11_DSV_DIMENSION_TEXTURE2DMS, 0 };
        CHK(ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)ms_dep, &d, &ms_ds));
    }
    else
    {
        /* keep fwd black so tonemap reads defined data */
        ID3D11RenderTargetView *f = rtv(fwd, DXGI_FORMAT_R16G16B16A16_FLOAT, 0);
        const float z[4] = { 0 };
        ID3D11DeviceContext_ClearRenderTargetView(ctx, f, z);
        ID3D11RenderTargetView_Release(f);
    }

    /* --- states --- */
    ID3D11SamplerState *s_lin, *s_cmp;
    {
        D3D11_SAMPLER_DESC d = { D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
                                 D3D11_TEXTURE_ADDRESS_CLAMP, 0, 1, D3D11_COMPARISON_NEVER, { 0 }, 0, D3D11_FLOAT32_MAX };
        CHK(ID3D11Device_CreateSamplerState(dev, &d, &s_lin));
        d.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        d.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
        CHK(ID3D11Device_CreateSamplerState(dev, &d, &s_cmp));
    }
    ID3D11DepthStencilState *ds_write, *ds_test, *ds_off;
    {
        D3D11_DEPTH_STENCIL_DESC d = { TRUE, D3D11_DEPTH_WRITE_MASK_ALL, D3D11_COMPARISON_LESS, TRUE, 0xff, 0xff,
                                       { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_ALWAYS },
                                       { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_ALWAYS } };
        CHK(ID3D11Device_CreateDepthStencilState(dev, &d, &ds_write));
        d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        d.StencilEnable = FALSE;
        CHK(ID3D11Device_CreateDepthStencilState(dev, &d, &ds_test));
        d.DepthEnable = FALSE;
        CHK(ID3D11Device_CreateDepthStencilState(dev, &d, &ds_off));
    }
    ID3D11BlendState *bl_alpha, *bl_add;
    {
        D3D11_BLEND_DESC d = { 0 };
        d.RenderTarget[0].BlendEnable = TRUE;
        d.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        d.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        d.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        d.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        d.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        d.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        d.RenderTarget[0].RenderTargetWriteMask = 0xf;
        CHK(ID3D11Device_CreateBlendState(dev, &d, &bl_alpha));
        d.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        d.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        CHK(ID3D11Device_CreateBlendState(dev, &d, &bl_add));
    }
    ID3D11RasterizerState *rs_shadow, *rs_ms;
    {
        D3D11_RASTERIZER_DESC d = { D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 100, 0, 1.5f, TRUE, FALSE, FALSE, FALSE };
        CHK(ID3D11Device_CreateRasterizerState(dev, &d, &rs_shadow));
        d.DepthBias = 0; d.SlopeScaledDepthBias = 0; d.MultisampleEnable = TRUE;
        CHK(ID3D11Device_CreateRasterizerState(dev, &d, &rs_ms));
    }

    /* --- queries --- */
    ID3D11Query *q_event[LATENCY], *q_dis[LATENCY], *q_t0[LATENCY], *q_t1[LATENCY], *q_occl[LATENCY];
    for (int i = 0; i < LATENCY; i++)
    {
        D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
        CHK(ID3D11Device_CreateQuery(dev, &qd, &q_event[i]));
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        CHK(ID3D11Device_CreateQuery(dev, &qd, &q_dis[i]));
        qd.Query = D3D11_QUERY_TIMESTAMP;
        CHK(ID3D11Device_CreateQuery(dev, &qd, &q_t0[i]));
        CHK(ID3D11Device_CreateQuery(dev, &qd, &q_t1[i]));
        qd.Query = D3D11_QUERY_OCCLUSION;
        CHK(ID3D11Device_CreateQuery(dev, &qd, &q_occl[i]));
    }

    const UINT strides[2] = { 12, 16 }, offs[2] = { 0, 0 };
    const float black[4] = { 0, 0, 0, 0 }, sky[4] = { 0.02f, 0.03f, 0.05f, 1 };
    const int total = WARMUP + frames;
    double *gpu = calloc(total, sizeof(double)), *wall = calloc(total, sizeof(double));
    int ngpu = 0, nwall = 0;
    double t_prev = 0, t_start = 0;

#define SET_CB(c) do { \
        if (update) ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)cb_draw, 0, NULL, &(c), 0, 0); \
        else { D3D11_MAPPED_SUBRESOURCE m_; ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)cb_draw, 0, D3D11_MAP_WRITE_DISCARD, 0, &m_); \
               memcpy(m_.pData, &(c), sizeof(c)); ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)cb_draw, 0); } } while (0)
#define VIEWPORT(w, h) do { D3D11_VIEWPORT vp_ = { 0, 0, (float)(w), (float)(h), 0, 1 }; ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp_); } while (0)
#define UNBIND_SRV() do { ID3D11ShaderResourceView *n_[8] = { 0 }; ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 8, n_); } while (0)

    for (int f = 0; f < total; f++)
    {
        int slot = f % LATENCY;
        if (f >= LATENCY)
        {
            while (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)q_event[slot], NULL, 0, 0) == S_FALSE) Sleep(0);
            if (ts)
            {
                D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
                UINT64 a, bb;
                while (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)q_dis[slot], &dj, sizeof(dj), 0) == S_FALSE) Sleep(0);
                while (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)q_t0[slot], &a, sizeof(a), 0) == S_FALSE) Sleep(0);
                while (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)q_t1[slot], &bb, sizeof(bb), 0) == S_FALSE) Sleep(0);
                if (f - LATENCY >= WARMUP && !dj.Disjoint && dj.Frequency && bb > a)
                    gpu[ngpu++] = (double)(bb - a) * 1000.0 / (double)dj.Frequency;
            }
        }
        double t = now_ms();
        if (f == WARMUP) t_start = t;
        if (f > WARMUP) wall[nwall++] = t - t_prev;
        t_prev = t;

        if (ts)
        {
            ID3D11DeviceContext_Begin(ctx, (ID3D11Asynchronous *)q_dis[slot]);
            ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)q_t0[slot]);
        }

        /* per frame constants */
        struct frame_cb fc = { { 0, 0, -2, 0 }, { 1.0f / 1.2f / ((float)W / H), 1.0f / 1.2f, 0, 0 },
                               { 0.25f, 0.4f, 0, 0.06f }, { (float)f * 0.01f, (float)lights, 0, 0 } };
        float zn = 0.5f, zf = 200.0f;
        fc.proj[2] = zf / (zf - zn);
        fc.proj[3] = -zn * zf / (zf - zn);
        {
            D3D11_MAPPED_SUBRESOURCE m;
            ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)cb_frame, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
            memcpy(m.pData, &fc, sizeof(fc));
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)cb_frame, 0);
        }
        ID3D11Buffer *cbs[2] = { cb_frame, cb_draw };
        ID3D11Buffer *vbs[2] = { vb, ivb };
        ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 2, cbs);
        ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 2, cbs);
        ID3D11DeviceContext_CSSetConstantBuffers(ctx, 0, 2, cbs);
        ID3D11SamplerState *smps[2] = { s_lin, s_cmp };
        ID3D11DeviceContext_PSSetSamplers(ctx, 0, 2, smps);
        ID3D11DeviceContext_IASetInputLayout(ctx, layout);
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 2, vbs, strides, offs);
        ID3D11DeviceContext_IASetIndexBuffer(ctx, ib, DXGI_FORMAT_R16_UINT, 0);

        /* shadow cascades: clear all slices first (shadow atlas style), then render each */
        for (int c = 0; c < CASCADES; c++)
            ID3D11DeviceContext_ClearDepthStencilView(ctx, shadow_ds[c], D3D11_CLEAR_DEPTH, 1.0f, 0);
        ID3D11DeviceContext_VSSetShader(ctx, vs_shadow, NULL, 0);
        ID3D11DeviceContext_PSSetShader(ctx, NULL, NULL, 0);
        ID3D11DeviceContext_RSSetState(ctx, rs_shadow);
        ID3D11DeviceContext_OMSetDepthStencilState(ctx, ds_write, 0);
        ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xffffffff);
        VIEWPORT(SHADOW, SHADOW);
        for (int c = 0; c < CASCADES; c++)
        {
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, shadow_ds[c]);
            fc.light[3] = 0.06f / (float)(1 << c);
            D3D11_MAPPED_SUBRESOURCE m;
            ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)cb_frame, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
            memcpy(m.pData, &fc, sizeof(fc));
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)cb_frame, 0);
            for (int d = 0; d < DRAWS_GB; d++)
                ID3D11DeviceContext_DrawIndexedInstanced(ctx, GRID * GRID * 6, INST_GB / DRAWS_GB, 0, 0, d * (INST_GB / DRAWS_GB));
        }
        fc.light[3] = 0.06f;
        {
            D3D11_MAPPED_SUBRESOURCE m;
            ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)cb_frame, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
            memcpy(m.pData, &fc, sizeof(fc));
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)cb_frame, 0);
        }

        /* g-buffer */
        if (!noclear)
            for (int i = 0; i < 4; i++) ID3D11DeviceContext_ClearRenderTargetView(ctx, gb_rt[i], black);
        ID3D11DeviceContext_ClearDepthStencilView(ctx, dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        if (poll && f)
        {
            /* a game checking the previous frame's GPU fence in the middle of recording (no DONOTFLUSH) */
            BOOL done;
            ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)q_event[(f + LATENCY - 1) % LATENCY], &done, sizeof(done), 0);
        }
        if (occl)
        {
            /* an occlusion query ended between the clears and the first draw */
            ID3D11DeviceContext_Begin(ctx, (ID3D11Asynchronous *)q_occl[slot]);
            ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)q_occl[slot]);
        }
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 4, gb_rt, dsv);
        ID3D11DeviceContext_RSSetState(ctx, NULL);
        VIEWPORT(W, H);
        ID3D11DeviceContext_OMSetDepthStencilState(ctx, ds_write, 1);
        ID3D11DeviceContext_VSSetShader(ctx, vs_gb, NULL, 0);
        ID3D11DeviceContext_PSSetShader(ctx, ps_gb, NULL, 0);
        {
            ID3D11ShaderResourceView *s[2] = { srv_albedo, srv_normal };
            ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, s);
        }
        for (int d = 0; d < DRAWS_GB; d++)
        {
            struct draw_cb dc = { { 0.6f + 0.4f * (float)(d & 1), 0.7f + 0.02f * (float)d, 0.9f - 0.03f * (float)d, 1 },
                                  { 0.5f, 0.2f + 0.05f * (float)(d & 3), (d % 5) ? 0.0f : 0.5f, 0 } };
            SET_CB(dc);
            ID3D11DeviceContext_DrawIndexedInstanced(ctx, GRID * GRID * 6, INST_GB / DRAWS_GB, 0, 0, d * (INST_GB / DRAWS_GB));
        }

        /* ssao: full-screen, depth taps into R8 (no clear, every pixel written) */
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &ao_rt, NULL);
        ID3D11DeviceContext_OMSetDepthStencilState(ctx, ds_off, 0);
        ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
        ID3D11DeviceContext_VSSetShader(ctx, vs_full, NULL, 0);
        ID3D11DeviceContext_PSSetShader(ctx, ps_ssao, NULL, 0);
        {
            ID3D11ShaderResourceView *s[5] = { NULL, NULL, NULL, NULL, depth_sr };
            ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 5, s);
        }
        ID3D11DeviceContext_Draw(ctx, 3, 0);
        UNBIND_SRV();

        /* lighting into HDR */
        if (!noclear) ID3D11DeviceContext_ClearRenderTargetView(ctx, hdr_rt, sky);
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &hdr_rt, NULL);
        ID3D11DeviceContext_OMSetDepthStencilState(ctx, ds_off, 0);
        ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
        ID3D11DeviceContext_VSSetShader(ctx, vs_full, NULL, 0);
        ID3D11DeviceContext_PSSetShader(ctx, ps_light, NULL, 0);
        {
            ID3D11ShaderResourceView *s[7] = { gb_sr[0], gb_sr[1], gb_sr[2], gb_sr[3], depth_sr, shadow_sr, ao_sr };
            ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 7, s);
        }
        ID3D11DeviceContext_Draw(ctx, 3, 0);
        UNBIND_SRV();
        if (discard)
            for (int i = 0; i < 4; i++) ID3D11DeviceContext1_DiscardView(ctx1, (ID3D11View *)gb_rt[i]);

        /* exposure (compute) */
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
        ID3D11DeviceContext_CSSetShader(ctx, cs_lum, NULL, 0);
        ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 1, &hdr_sr);
        ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &lum_uav, NULL);
        ID3D11DeviceContext_Dispatch(ctx, lw, lh, 1);
        {
            ID3D11ShaderResourceView *n = NULL;
            ID3D11UnorderedAccessView *nu = NULL;
            ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 1, &n);
            ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &nu, NULL);
        }

        /* translucent on HDR with read-only depth */
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &hdr_rt, dsv_ro);
        ID3D11DeviceContext_OMSetDepthStencilState(ctx, ds_test, 0);
        ID3D11DeviceContext_OMSetBlendState(ctx, bl_alpha, NULL, 0xffffffff);
        ID3D11DeviceContext_IASetInputLayout(ctx, layout);
        {
            ID3D11Buffer *tv[2] = { vb, tivb };
            ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 2, tv, strides, offs);
        }
        ID3D11DeviceContext_VSSetShader(ctx, vs_trans, NULL, 0);
        ID3D11DeviceContext_PSSetShader(ctx, ps_trans, NULL, 0);
        for (int d = 0; d < TRANS_DRAWS; d++)
        {
            struct draw_cb dc = { { 0.3f + 0.1f * (float)d, 0.5f, 0.9f - 0.1f * (float)d, 1 }, { 0 } };
            SET_CB(dc);
            ID3D11DeviceContext_DrawIndexedInstanced(ctx, GRID * GRID * 6, 64 / TRANS_DRAWS, 0, 0, d * (64 / TRANS_DRAWS));
        }
        if (discard) ID3D11DeviceContext1_DiscardView(ctx1, (ID3D11View *)dsv);

        /* msaa forward + resolve */
        if (msaa)
        {
            ID3D11DeviceContext_ClearRenderTargetView(ctx, ms_rt, black);
            ID3D11DeviceContext_ClearDepthStencilView(ctx, ms_ds, D3D11_CLEAR_DEPTH, 1.0f, 0);
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &ms_rt, ms_ds);
            ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xffffffff);
            ID3D11DeviceContext_OMSetDepthStencilState(ctx, ds_write, 0);
            ID3D11DeviceContext_RSSetState(ctx, rs_ms);
            ID3D11DeviceContext_PSSetShader(ctx, ps_fwd, NULL, 0);
            ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &srv_albedo);
            for (int d = 0; d < TRANS_DRAWS; d++)
            {
                struct draw_cb dc = { { 0.9f, 0.6f + 0.05f * (float)d, 0.4f, 1 }, { 0 } };
                SET_CB(dc);
                ID3D11DeviceContext_DrawIndexedInstanced(ctx, GRID * GRID * 6, 64 / TRANS_DRAWS, 0, 0, d * (64 / TRANS_DRAWS));
            }
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
            ID3D11DeviceContext_ResolveSubresource(ctx, (ID3D11Resource *)fwd, 0, (ID3D11Resource *)ms_col, 0, DXGI_FORMAT_R16G16B16A16_FLOAT);
            if (discard)
            {
                ID3D11DeviceContext1_DiscardView(ctx1, (ID3D11View *)ms_rt);
                ID3D11DeviceContext1_DiscardView(ctx1, (ID3D11View *)ms_ds);
            }
            ID3D11DeviceContext_RSSetState(ctx, NULL);
        }

        /* taa: current HDR + history into this frame's TAA target (ping-pong) */
        ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
        ID3D11DeviceContext_VSSetShader(ctx, vs_full, NULL, 0);
        ID3D11DeviceContext_OMSetDepthStencilState(ctx, ds_off, 0);
        ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xffffffff);
        ID3D11DeviceContext_RSSetState(ctx, NULL);
        VIEWPORT(W, H);
        ID3D11ShaderResourceView *scene_sr = hdr_sr;
        if (!notaa)
        {
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &taa_rt[f & 1], NULL);
            ID3D11DeviceContext_PSSetShader(ctx, ps_taa, NULL, 0);
            struct draw_cb dc = { { 0 }, { f ? 1.0f : 0.0f, 0, 0, 0 } };
            SET_CB(dc);
            ID3D11ShaderResourceView *s[2] = { hdr_sr, taa_sr[(f + 1) & 1] };
            ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, s);
            ID3D11DeviceContext_Draw(ctx, 3, 0);
            UNBIND_SRV();
            scene_sr = taa_sr[f & 1];
        }

        /* bloom: down chain, then additive up chain */
        ID3D11DeviceContext_PSSetShader(ctx, ps_down, NULL, 0);
        for (int i = 0; i < BLOOM; i++)
        {
            ID3D11ShaderResourceView *in = i ? bloom_sr[i - 1] : scene_sr;
            UINT iw = i ? bw[i - 1] : W, ih = i ? bh[i - 1] : H;
            if (discard) ID3D11DeviceContext1_DiscardView(ctx1, (ID3D11View *)bloom_rt[i]);
            if (!noclear) ID3D11DeviceContext_ClearRenderTargetView(ctx, bloom_rt[i], black);
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &bloom_rt[i], NULL);
            VIEWPORT(bw[i], bh[i]);
            struct draw_cb dc = { { 0 }, { 0.5f / (float)iw, 0.5f / (float)ih, i ? 0.0f : 1.0f, 0 } };
            SET_CB(dc);
            ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &in);
            ID3D11DeviceContext_Draw(ctx, 3, 0);
            UNBIND_SRV();
        }
        ID3D11DeviceContext_PSSetShader(ctx, ps_up, NULL, 0);
        ID3D11DeviceContext_OMSetBlendState(ctx, bl_add, NULL, 0xffffffff);
        for (int i = BLOOM - 1; i > 0; i--)
        {
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &bloom_rt[i - 1], NULL);
            VIEWPORT(bw[i - 1], bh[i - 1]);
            struct draw_cb dc = { { 0 }, { 1.0f / (float)bw[i], 1.0f / (float)bh[i], 0, 0.7f } };
            SET_CB(dc);
            ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &bloom_sr[i]);
            ID3D11DeviceContext_Draw(ctx, 3, 0);
            UNBIND_SRV();
        }

        /* tonemap + UI */
        ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xffffffff);
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &ldr_rt, NULL);
        VIEWPORT(W, H);
        ID3D11DeviceContext_PSSetShader(ctx, ps_tone, NULL, 0);
        {
            struct draw_cb dc = { { 0 }, { msaa ? 1.0f : 0.0f, 0, 0, 0 } };
            SET_CB(dc);
            ID3D11ShaderResourceView *s[4] = { scene_sr, bloom_sr[0], lum_sr, fwd_sr };
            ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 4, s);
        }
        ID3D11DeviceContext_Draw(ctx, 3, 0);
        UNBIND_SRV();
        ID3D11DeviceContext_OMSetBlendState(ctx, bl_alpha, NULL, 0xffffffff);
        ID3D11DeviceContext_PSSetShader(ctx, ps_ui, NULL, 0);
        for (int u = 0; u < 4; u++)
        {
            D3D11_VIEWPORT vp = { 40.0f + 300.0f * (float)u, (float)H - 140.0f, 260, 100, 0, 1 };
            ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
            struct draw_cb dc = { { 0.2f * (float)u, 0.6f, 0.3f, 1 }, { 0 } };
            SET_CB(dc);
            ID3D11DeviceContext_Draw(ctx, 3, 0);
        }
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);

        if (ts)
        {
            ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)q_t1[slot]);
            ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)q_dis[slot]);
        }
        ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)q_event[slot]);
        ID3D11DeviceContext_Flush(ctx);
    }
    for (int i = 0; i < LATENCY; i++)
        while (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)q_event[i], NULL, 0, 0) == S_FALSE) Sleep(0);
    double t_end = now_ms();

    uint64_t h_ldr = readback(ldr, W * 4, dump, "ldr");
    uint64_t h_hdr = readback(hdr, W * 8, dump, "hdr");
    uint64_t h_dep = discard ? 0 : readback(depth, W * 8, dump, "depth");

    double gpu_med = median(gpu, ngpu), wall_med = median(wall, nwall);
    double gpu_min = ngpu ? gpu[0] : 0, gpu_p10 = ngpu ? gpu[ngpu / 10] : 0; /* sorted by median() */
    printf("{\"bench\":\"gpu_headless\",\"opts\":\"%s\",\"res\":\"%dx%d\",\"frames\":%d,\"gpu_ms\":%.3f,\"gpu_min_ms\":%.3f,"
           "\"gpu_p10_ms\":%.3f,\"wall_ms\":%.3f,\"avg_ms\":%.3f,\"hash_ldr\":\"%016llx\",\"hash_hdr\":\"%016llx\",\"hash_depth\":\"%016llx\"}\n",
           g_opts, W, H, frames, gpu_med, gpu_min, gpu_p10, wall_med, (t_end - t_start) / frames, (unsigned long long)h_ldr,
           (unsigned long long)h_hdr, (unsigned long long)h_dep);
    fflush(stdout);
    return 0;
}
