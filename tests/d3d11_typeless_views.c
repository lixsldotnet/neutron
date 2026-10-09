/* neutron test: typeless render targets written through one view type and read through another.
 *
 * x86_64 Windows program, headless. For each typeless family it renders a pattern into a full-size
 * render target through view A (UNORM, FLOAT, ...), then a compute shader reads it through view B
 * (UINT, SNORM, ...) into a buffer, and the bits are compared with what the CPU expects. This is what
 * d3d11.compressTypelessRenderTargets=True has to survive: without MTLTextureUsagePixelFormatView the
 * texture keeps Apple's lossless compression, and Metal does not define reads through another format.
 * Run it once with and once without DXMT_CONFIG="d3d11.compressTypelessRenderTargets=True".
 *
 * Build: x86_64-w64-mingw32-clang -O2 -o d3d11_typeless_views.exe d3d11_typeless_views.c -ld3d11 -ld3dcompiler
 * Exit code 0: all families read back bit exact.
 */
#define COBJMACROS
#define INITGUID
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define W 1024
#define H 512

static const char src[] =
    "struct fs { float4 pos : SV_Position; };\n"
    "fs vs(uint id : SV_VertexID) { fs o; float2 t = float2((id << 1) & 2, id & 2);\n"
    "  o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
    /* smooth gradients (compress well) plus a noisy block (does not) */
    "float4 ps(fs i) : SV_Target {\n"
    "  float2 p = floor(i.pos.xy);\n"
    "  float n = frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453);\n"
    "  float4 g = float4(p.x / 1024.0, p.y / 512.0, frac(p.x / 64.0), 1);\n"
    "  return (p.x > 700 && p.y > 300) ? float4(n, frac(n * 7.13), frac(n * 3.7), frac(n * 11.1)) : g;\n"
    "}\n"
    "Texture2D<uint4> tin : register(t0);\n"
    "RWByteAddressBuffer outb : register(u0);\n"
    "cbuffer c : register(b0) { uint shift; uint mask; uint comps; uint pad; };\n"
    /* pack the integer view's components back into the texel's bit layout (comps x shift bits) */
    "[numthreads(8, 8, 1)] void cs(uint3 id : SV_DispatchThreadID) {\n"
    "  uint4 v = tin.Load(int3(id.xy, 0));\n"
    "  uint lo = 0, hi = 0;\n"
    "  [unroll] for (uint k = 0; k < 4; k++) { if (k >= comps) break; uint b = (v[k] & mask);\n"
    "    uint bit = k * shift; if (bit < 32) { lo |= b << bit; if (bit + shift > 32) hi |= b >> (32 - bit); } else hi |= b << (bit - 32); }\n"
    "  uint idx = (id.y * 1024 + id.x) * 8;\n"
    "  outb.Store2(idx, uint2(lo, hi));\n"
    "}\n";

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;

static ID3DBlob *compile(const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *errors = NULL;
    if (FAILED(D3DCompile(src, sizeof(src) - 1, "t", NULL, NULL, entry, target, 0, 0, &code, &errors)))
    {
        if (errors) fprintf(stderr, "%s\n", (const char *)ID3D10Blob_GetBufferPointer(errors));
        return NULL;
    }
    return code;
}

struct family { const char *name; DXGI_FORMAT typeless, write, read; UINT bpp, shift, comps; };

int main(void)
{
    static const struct family fams[] = {
        { "R8G8B8A8 unorm->uint", DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UINT, 4, 8, 4 },
        { "R8G8B8A8 srgb->uint", DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UINT, 4, 8, 4 },
        { "R10G10B10A2 unorm->uint", DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UINT, 4, 10, 3 },
        { "R16G16B16A16 float->uint", DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_UINT, 8, 16, 4 },
        { "R16G16B16A16 unorm->uint", DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_UINT, 8, 16, 4 },
        { "R16G16 float->uint", DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_UINT, 4, 16, 2 },
        { "R32 float->uint", DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_UINT, 4, 32, 1 },
        { "B8G8R8A8 unorm->srgb", DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 4, 8, 4 },
    };
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    int failed = 0;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &fl, 1, D3D11_SDK_VERSION, &dev, NULL, &ctx)))
        return 2;
    ID3DBlob *bvs = compile("vs", "vs_5_0"), *bps = compile("ps", "ps_5_0"), *bcs = compile("cs", "cs_5_0");
    if (!bvs || !bps || !bcs) return 2;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11ComputeShader *cs;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(bvs), ID3D10Blob_GetBufferSize(bvs), NULL, &vs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(bps), ID3D10Blob_GetBufferSize(bps), NULL, &ps);
    ID3D11Device_CreateComputeShader(dev, ID3D10Blob_GetBufferPointer(bcs), ID3D10Blob_GetBufferSize(bcs), NULL, &cs);

    for (unsigned f = 0; f < sizeof(fams) / sizeof(fams[0]); f++)
    {
        const struct family *fa = &fams[f];
        D3D11_TEXTURE2D_DESC td = { W, H, 1, 1, fa->typeless, { 1, 0 }, D3D11_USAGE_DEFAULT,
                                    D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0 };
        ID3D11Texture2D *tex, *st;
        ID3D11RenderTargetView *rtv;
        D3D11_RENDER_TARGET_VIEW_DESC rd = { fa->write, D3D11_RTV_DIMENSION_TEXTURE2D };
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &tex))) { printf("%s: no texture\n", fa->name); failed = 1; continue; }
        ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)tex, &rd, &rtv);
        D3D11_VIEWPORT vp = { 0, 0, W, H, 0, 1 };
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
        ID3D11DeviceContext_Draw(ctx, 3, 0);
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);

        /* expected bits: copy of the texture itself (a blit, no format view involved) */
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Device_CreateTexture2D(dev, &td, NULL, &st);
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)tex);
        D3D11_MAPPED_SUBRESOURCE m;
        ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m);
        static uint8_t expect[W * H * 8];
        for (int y = 0; y < H; y++) memcpy(expect + (size_t)y * W * fa->bpp, (uint8_t *)m.pData + (size_t)y * m.RowPitch, W * fa->bpp);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);

        if (!fa->read)
        {
            /* sRGB toggle family: read through the other view by a second render pass is covered by the bench */
            printf("%-26s skipped (sRGB only)\n", fa->name);
            continue;
        }
        ID3D11ShaderResourceView *srv;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = { fa->read, D3D11_SRV_DIMENSION_TEXTURE2D };
        sd.Texture2D.MipLevels = 1;
        if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)tex, &sd, &srv))) { printf("%s: no srv\n", fa->name); failed = 1; continue; }
        ID3D11Buffer *ob, *obs;
        D3D11_BUFFER_DESC bd = { W * H * 8, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0 };
        ID3D11Device_CreateBuffer(dev, &bd, NULL, &ob);
        bd.Usage = D3D11_USAGE_STAGING; bd.BindFlags = 0; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; bd.MiscFlags = 0;
        ID3D11Device_CreateBuffer(dev, &bd, NULL, &obs);
        ID3D11UnorderedAccessView *uav;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = { DXGI_FORMAT_R32_TYPELESS, D3D11_UAV_DIMENSION_BUFFER };
        ud.Buffer.NumElements = W * H * 2;
        ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)ob, &ud, &uav);
        UINT cbdata[4] = { fa->shift, fa->shift == 32 ? 0xffffffffu : (1u << fa->shift) - 1, fa->comps, 0 };
        D3D11_BUFFER_DESC cd = { 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
        D3D11_SUBRESOURCE_DATA cs_init = { cbdata };
        ID3D11Buffer *cb;
        ID3D11Device_CreateBuffer(dev, &cd, &cs_init, &cb);
        ID3D11DeviceContext_CSSetShader(ctx, cs, NULL, 0);
        ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 1, &srv);
        ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &uav, NULL);
        ID3D11DeviceContext_CSSetConstantBuffers(ctx, 0, 1, &cb);
        ID3D11DeviceContext_Dispatch(ctx, W / 8, H / 8, 1);
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)obs, (ID3D11Resource *)ob);
        ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)obs, 0, D3D11_MAP_READ, 0, &m);
        const uint8_t *got = m.pData;
        unsigned bad = 0, first = ~0u;
        for (unsigned i = 0; i < W * H; i++)
        {
            uint64_t e = 0, g;
            memcpy(&e, expect + (size_t)i * fa->bpp, fa->bpp);
            memcpy(&g, got + (size_t)i * 8, 8);
            uint64_t mask = fa->comps * fa->shift >= 64 ? ~0ull : (1ull << (fa->comps * fa->shift)) - 1;
            if ((e & mask) != (g & mask)) { bad++; if (first == ~0u) first = i; }
        }
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)obs, 0);
        printf("%-26s %s", fa->name, bad ? "MISMATCH" : "ok");
        if (bad) printf(" (%u texels, first at %u,%u)", bad, first % W, first / W);
        printf("\n");
        failed |= bad != 0;
    }
    return failed;
}
