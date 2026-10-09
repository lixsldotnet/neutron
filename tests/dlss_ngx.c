/* neutron test: DLSS through DXMT's NGX core on the MetalFX temporal scaler, headless.
 *
 * Does what a D3D11 or D3D12 game with DLSS does, without NVIDIA's SDK library:
 *   - DXGI adapter vendor (NVIDIA 0x10DE with NEUTRON_DLSS=1)
 *   - nvapi64.dll: nvapi_QueryInterface, NvAPI_Initialize, EnumPhysicalGPUs, GetFullName
 *   - NGX core: HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore FullPath, then
 *     <FullPath>\_nvngx.dll, as the NGX loader in the game does
 *   - NVSDK_NGX_D3D11_ (or _D3D12_) Init_with_ProjectID, GetCapabilityParameters,
 *     SuperSampling.Available, the DLSSOptimalSettingsCallback (render size for a mode)
 *   - CreateFeature (DLSS, MVLowRes) and Evaluate with color (RGBA16F), depth (R32F),
 *     motion vectors (RG16F) and Halton jitter on offscreen textures, output RGBA16F;
 *     D3D12 records it into the command list with the uploads, one ExecuteCommandLists
 *     per frame
 *
 * The scene is analytic (zone plate, checkerboard, thin lines), so the output can be
 * compared with the exact picture at display size. Phase 1 is a still scene: after 32
 * jittered frames the output has to be clearly closer to the exact picture than a
 * bilinear upscale of one frame (temporal upscaling adds detail). Phase 2 scrolls the
 * scene with motion vectors: the output has to stay close as well (no smearing).
 * The parameter object is called through its vtable in the layout MSVC gives
 * NVSDK_NGX_Parameter (overloads grouped, in reverse order), like a game binary does.
 *
 * Usage: dlss_ngx.exe            expects DLSS (NEUTRON_DLSS=1), exit 0 when it works
 *        dlss_ngx.exe d3d12      the same through D3D12
 *        dlss_ngx.exe off        expects no DLSS (no NVIDIA vendor id, no NGX core)
 *        options: nosubrect (render subrect 0, as games with older SDKs), noae (no
 *                 auto exposure flag), jneg / mvneg (negated jitter / motion vectors, to
 *                 see the sign conventions: both make the output clearly worse),
 *                 noeval (no Evaluate calls), dump=<prefix> (writes <prefix>.out.ppm,
 *                 .moving.ppm, .truth.ppm, .bilinear.ppm; Windows path, e.g. Z:/tmp/x)
 * Build: x86_64-w64-mingw32-clang -O2 -o dlss_ngx.exe dlss_ngx.c -ld3d11 -ld3d12 -ldxgi -ladvapi32
 */
#define COBJMACROS
#define INITGUID
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DISPLAY_W 512
#define DISPLAY_H 512

/* NGX ABI (values from the public DLSS SDK headers) */
#define NGX_SUCCEED(r) (((r) & 0xFFF00000u) != 0xBAD00000u)
#define NGX_FEATURE_SUPERSAMPLING 1
#define NGX_PERF_MAXPERF 0
#define NGX_FLAG_MV_LOWRES (1 << 1)
#define NGX_FLAG_AUTO_EXPOSURE (1 << 6)

typedef struct NGXParam NGXParam;
typedef struct NGXHandle { unsigned int Id; } NGXHandle;
/* MSVC layout of NVSDK_NGX_Parameter: the overloads of Set and of Get are grouped
   and in reverse declaration order (checked with clang -target x86_64-pc-windows-msvc). */
typedef struct NGXParamVtbl {
    void (*SetVoidPointer)(NGXParam *, const char *, void *);
    void (*SetD3d12Resource)(NGXParam *, const char *, void *);
    void (*SetD3d11Resource)(NGXParam *, const char *, ID3D11Resource *);
    void (*SetI)(NGXParam *, const char *, int);
    void (*SetUI)(NGXParam *, const char *, unsigned int);
    void (*SetD)(NGXParam *, const char *, double);
    void (*SetF)(NGXParam *, const char *, float);
    void (*SetULL)(NGXParam *, const char *, unsigned long long);
    unsigned int (*GetVoidPointer)(NGXParam *, const char *, void **);
    unsigned int (*GetD3d12Resource)(NGXParam *, const char *, void **);
    unsigned int (*GetD3d11Resource)(NGXParam *, const char *, ID3D11Resource **);
    unsigned int (*GetI)(NGXParam *, const char *, int *);
    unsigned int (*GetUI)(NGXParam *, const char *, unsigned int *);
    unsigned int (*GetD)(NGXParam *, const char *, double *);
    unsigned int (*GetF)(NGXParam *, const char *, float *);
    unsigned int (*GetULL)(NGXParam *, const char *, unsigned long long *);
    void (*Reset)(NGXParam *);
} NGXParamVtbl;
struct NGXParam { const NGXParamVtbl *v; };

/* device: ID3D11Device or ID3D12Device, context: ID3D11DeviceContext or ID3D12GraphicsCommandList */
typedef unsigned int (*PFN_Init_with_ProjectID)(const char *, int, const char *, const wchar_t *, void *device,
                                                const void *, unsigned int);
typedef unsigned int (*PFN_GetParams)(NGXParam **);
typedef unsigned int (*PFN_CreateFeature)(void *context, unsigned int, NGXParam *, NGXHandle **);
typedef unsigned int (*PFN_EvaluateFeature)(void *context, const NGXHandle *, NGXParam *, void *);
typedef unsigned int (*PFN_ReleaseFeature)(NGXHandle *);
typedef unsigned int (*PFN_DestroyParameters)(NGXParam *);
typedef unsigned int (*PFN_Shutdown1)(void *device);
typedef unsigned int (*PFN_OptimalSettings)(NGXParam *);

/* NvAPI through nvapi_QueryInterface ids (as nvapi.lib does) */
typedef void *(*PFN_nvapi_QueryInterface)(unsigned int);
typedef int (*PFN_NvAPI_Initialize)(void);
typedef int (*PFN_NvAPI_EnumPhysicalGPUs)(void *handles[64], unsigned int *count);
typedef int (*PFN_NvAPI_GPU_GetFullName)(void *gpu, char name[64]);

static int g_dump_set;
static char g_dump[MAX_PATH];

static int fail(const char *what, unsigned long code)
{
    printf("FAIL %s: 0x%08lx\n", what, code);
    return 1;
}

/* ---- half floats ---- */
static unsigned short f2h(float f)
{
    unsigned int x;
    memcpy(&x, &f, 4);
    unsigned int sign = (x >> 16) & 0x8000, mant = x & 0x7fffff;
    int exp = (int)((x >> 23) & 0xff) - 127 + 15;
    if (exp <= 0) return (unsigned short)sign;
    if (exp >= 31) return (unsigned short)(sign | 0x7c00);
    return (unsigned short)(sign | (exp << 10) | ((mant + 0x1000) >> 13));
}

static float h2f(unsigned short h)
{
    unsigned int sign = (h & 0x8000u) << 16, exp = (h >> 10) & 0x1f, mant = h & 0x3ff, x;
    if (exp == 0) x = sign; /* flush denormals */
    else if (exp == 31) x = sign | 0x7f800000 | (mant << 13);
    else x = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    float f;
    memcpy(&f, &x, 4);
    return f;
}

/* ---- the scene, in display pixel coordinates ---- */
static void scene(double x, double y, float rgb[3])
{
    /* zone plate in the left half: frequencies up to the display Nyquist limit */
    double cx = x - DISPLAY_W * 0.25, cy = y - DISPLAY_H * 0.5;
    double zp = 0.5 + 0.5 * cos(M_PI * (cx * cx + cy * cy) / (DISPLAY_W * 0.5) * 0.5);
    /* checkerboard of 6 pixel squares in the right half, with thin dark lines */
    int cb = ((int)floor(x / 6.0) + (int)floor(y / 6.0)) & 1;
    double line = (fmod(fabs(x + y * 0.37), 23.0) < 1.0) ? 0.0 : 1.0;
    if (x < DISPLAY_W * 0.5) {
        rgb[0] = (float)zp; rgb[1] = (float)zp; rgb[2] = (float)zp;
    } else {
        rgb[0] = (float)((cb ? 0.9 : 0.15) * line);
        rgb[1] = (float)((cb ? 0.6 : 0.25) * line);
        rgb[2] = (float)((cb ? 0.2 : 0.7) * line);
    }
}

/* exact picture at display size (8x8 box supersampling), content moved by (ox, oy) */
static void truth_image(float *out, double ox, double oy)
{
    for (int y = 0; y < DISPLAY_H; y++)
        for (int x = 0; x < DISPLAY_W; x++) {
            float acc[3] = { 0 }, s[3];
            for (int j = 0; j < 8; j++)
                for (int i = 0; i < 8; i++) {
                    scene(x + (i + 0.5) / 8.0 - ox, y + (j + 0.5) / 8.0 - oy, s);
                    acc[0] += s[0]; acc[1] += s[1]; acc[2] += s[2];
                }
            for (int c = 0; c < 3; c++) out[(y * DISPLAY_W + x) * 3 + c] = acc[c] / 64.0f;
        }
}

/* one rendered frame: one sample per render pixel at center - jitter (the projection
   matrix moves the picture by +jitter pixels), content moved by (ox, oy) display pixels */
static void render_frame(unsigned short *rgba16f, unsigned rw, unsigned rh, double jx, double jy, double ox, double oy)
{
    double sx = (double)DISPLAY_W / rw, sy = (double)DISPLAY_H / rh;
    float s[3];
    for (unsigned y = 0; y < rh; y++)
        for (unsigned x = 0; x < rw; x++) {
            scene((x + 0.5 - jx) * sx - ox, (y + 0.5 - jy) * sy - oy, s);
            unsigned short *p = rgba16f + (y * rw + x) * 4;
            p[0] = f2h(s[0]); p[1] = f2h(s[1]); p[2] = f2h(s[2]); p[3] = f2h(1.0f);
        }
}

static void bilinear(const unsigned short *src, unsigned rw, unsigned rh, float *out)
{
    for (int y = 0; y < DISPLAY_H; y++)
        for (int x = 0; x < DISPLAY_W; x++) {
            double u = (x + 0.5) * rw / DISPLAY_W - 0.5, v = (y + 0.5) * rh / DISPLAY_H - 0.5;
            int x0 = (int)floor(u), y0 = (int)floor(v);
            double fx = u - x0, fy = v - y0;
            for (int c = 0; c < 3; c++) {
                double acc = 0;
                for (int k = 0; k < 4; k++) {
                    int xi = x0 + (k & 1), yi = y0 + (k >> 1);
                    xi = xi < 0 ? 0 : xi >= (int)rw ? (int)rw - 1 : xi;
                    yi = yi < 0 ? 0 : yi >= (int)rh ? (int)rh - 1 : yi;
                    double w = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
                    acc += w * h2f(src[(yi * rw + xi) * 4 + c]);
                }
                out[(y * DISPLAY_W + x) * 3 + c] = (float)acc;
            }
        }
}

/* mean absolute error per channel in 0..255, without an 8 pixel border */
static double image_error(const float *a, const float *b)
{
    double sum = 0;
    long n = 0;
    for (int y = 8; y < DISPLAY_H - 8; y++)
        for (int x = 8; x < DISPLAY_W - 8; x++)
            for (int c = 0; c < 3; c++, n++) {
                double av = a[(y * DISPLAY_W + x) * 3 + c], bv = b[(y * DISPLAY_W + x) * 3 + c];
                av = av < 0 ? 0 : av > 1 ? 1 : av;
                bv = bv < 0 ? 0 : bv > 1 ? 1 : bv;
                sum += fabs(av - bv);
            }
    return sum / n * 255.0;
}

static void dump_ppm(const char *suffix, const float *img)
{
    if (!g_dump_set) return;
    char path[MAX_PATH + 32];
    snprintf(path, sizeof(path), "%s.%s.ppm", g_dump, suffix);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", DISPLAY_W, DISPLAY_H);
    for (int i = 0; i < DISPLAY_W * DISPLAY_H * 3; i++) {
        float v = img[i] < 0 ? 0 : img[i] > 1 ? 1 : img[i];
        fputc((int)(v * 255.0f + 0.5f), f);
    }
    fclose(f);
}

static double halton(int i, int b)
{
    double f = 1, r = 0;
    for (; i > 0; i /= b) { f /= b; r += f * (i % b); }
    return r;
}

static ID3D11Texture2D *make_texture(ID3D11Device *dev, unsigned w, unsigned h, DXGI_FORMAT fmt, UINT bind)
{
    D3D11_TEXTURE2D_DESC td = { 0 };
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = fmt;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = bind;
    ID3D11Texture2D *t = NULL;
    HRESULT hr = ID3D11Device_CreateTexture2D(dev, &td, NULL, &t);
    if (FAILED(hr)) printf("CreateTexture2D %ux%u fmt %u: 0x%08lx\n", w, h, fmt, (unsigned long)hr);
    return t;
}

static int read_output(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *output, float *img)
{
    ID3D11Texture2D *staging = NULL;
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D_GetDesc(output, &td);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &staging))) return 0;
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)output);
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &map))) return 0;
    for (int y = 0; y < DISPLAY_H; y++) {
        const unsigned short *row = (const unsigned short *)((const char *)map.pData + y * map.RowPitch);
        for (int x = 0; x < DISPLAY_W; x++)
            for (int c = 0; c < 3; c++) img[(y * DISPLAY_W + x) * 3 + c] = h2f(row[x * 4 + c]);
    }
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);
    ID3D11Texture2D_Release(staging);
    return 1;
}

/* ---- D3D12: device, one direct queue, uploads through an upload buffer ---- */
#define ALIGN(v, a) (((v) + (a) - 1) & ~((a) - 1))

typedef struct D12 {
    ID3D12Device *dev;
    ID3D12CommandQueue *queue;
    ID3D12CommandAllocator *alloc;
    ID3D12GraphicsCommandList *list;
    ID3D12Fence *fence;
    UINT64 fence_value;
    HANDLE event;
    ID3D12Resource *upload, *readback;
    unsigned char *upload_ptr;
} D12;

static ID3D12Resource *d12_resource(D12 *d, D3D12_HEAP_TYPE heap, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp = { 0 };
    hp.Type = heap;
    ID3D12Resource *r = NULL;
    HRESULT hr = ID3D12Device_CreateCommittedResource(d->dev, &hp, D3D12_HEAP_FLAG_NONE, desc, state, NULL,
                                                      &IID_ID3D12Resource, (void **)&r);
    if (FAILED(hr)) printf("CreateCommittedResource: 0x%08lx\n", (unsigned long)hr);
    return r;
}

static ID3D12Resource *d12_texture(D12 *d, unsigned w, unsigned h, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_RESOURCE_DESC rd = { 0 };
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = fmt;
    rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; rd.Flags = flags;
    return d12_resource(d, D3D12_HEAP_TYPE_DEFAULT, &rd, D3D12_RESOURCE_STATE_COMMON);
}

static ID3D12Resource *d12_buffer(D12 *d, D3D12_HEAP_TYPE heap, UINT64 size)
{
    D3D12_RESOURCE_DESC rd = { 0 };
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d12_resource(d, heap, &rd, heap == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST);
}

/* buffer <-> texture copy with a placed footprint */
static void d12_copy(D12 *d, ID3D12Resource *buffer, UINT64 offset, UINT pitch, DXGI_FORMAT fmt, unsigned w, unsigned h,
                     ID3D12Resource *tex, int to_texture)
{
    D3D12_TEXTURE_COPY_LOCATION t = { 0 }, b = { 0 };
    t.pResource = tex; t.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; t.SubresourceIndex = 0;
    b.pResource = buffer; b.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    b.PlacedFootprint.Offset = offset;
    b.PlacedFootprint.Footprint.Format = fmt;
    b.PlacedFootprint.Footprint.Width = w; b.PlacedFootprint.Footprint.Height = h;
    b.PlacedFootprint.Footprint.Depth = 1; b.PlacedFootprint.Footprint.RowPitch = pitch;
    if (to_texture) ID3D12GraphicsCommandList_CopyTextureRegion(d->list, &t, 0, 0, 0, &b, NULL);
    else ID3D12GraphicsCommandList_CopyTextureRegion(d->list, &b, 0, 0, 0, &t, NULL);
}

static void d12_barrier(D12 *d, ID3D12Resource *r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b = { 0 };
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before; b.Transition.StateAfter = after;
    ID3D12GraphicsCommandList_ResourceBarrier(d->list, 1, &b);
}

/* close, execute, wait, reopen */
static int d12_submit(D12 *d)
{
    if (FAILED(ID3D12GraphicsCommandList_Close(d->list))) return 0;
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)d->list };
    ID3D12CommandQueue_ExecuteCommandLists(d->queue, 1, lists);
    ID3D12CommandQueue_Signal(d->queue, d->fence, ++d->fence_value);
    ID3D12Fence_SetEventOnCompletion(d->fence, d->fence_value, d->event);
    if (WaitForSingleObject(d->event, 20000) != WAIT_OBJECT_0) return 0;
    ID3D12CommandAllocator_Reset(d->alloc);
    ID3D12GraphicsCommandList_Reset(d->list, d->alloc, NULL);
    return 1;
}

/* ---- the test ---- */
typedef struct Options {
    int d3d12, expect_off, jneg, mvneg, noeval, nosubrect, noae;
} Options;

static unsigned short *g_frame, *g_mv_data;
static float *g_depth_data;

int main(int argc, char **argv)
{
    Options o = { 0 };
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "off")) o.expect_off = 1;
        else if (!strcmp(argv[i], "d3d12")) o.d3d12 = 1;
        else if (!strcmp(argv[i], "jneg")) o.jneg = 1;
        else if (!strcmp(argv[i], "mvneg")) o.mvneg = 1;
        else if (!strcmp(argv[i], "noeval")) o.noeval = 1;
        else if (!strcmp(argv[i], "nosubrect")) o.nosubrect = 1;
        else if (!strcmp(argv[i], "noae")) o.noae = 1;
        else if (!strncmp(argv[i], "dump=", 5)) { g_dump_set = 1; snprintf(g_dump, sizeof(g_dump), "%s", argv[i] + 5); }
    }
    const char *api = o.d3d12 ? "D3D12" : "D3D11";
    HRESULT hr;

    /* 1. device, adapter vendor (games check it before they offer DLSS) */
    ID3D11Device *dev11 = NULL;
    ID3D11DeviceContext *ctx = NULL;
    D12 d = { 0 };
    DXGI_ADAPTER_DESC ad = { 0 };
    if (o.d3d12) {
        IDXGIFactory4 *factory = NULL;
        IDXGIAdapter1 *adapter = NULL;
        if (FAILED(hr = CreateDXGIFactory1(&IID_IDXGIFactory4, (void **)&factory)))
            return fail("CreateDXGIFactory1", hr);
        if (FAILED(hr = IDXGIFactory4_EnumAdapters1(factory, 0, &adapter)))
            return fail("EnumAdapters1", hr);
        IDXGIAdapter1_GetDesc(adapter, &ad);
        if (FAILED(hr = D3D12CreateDevice((IUnknown *)adapter, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&d.dev)))
            return fail("D3D12CreateDevice", hr);
        D3D12_COMMAND_QUEUE_DESC qd = { 0 };
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(hr = ID3D12Device_CreateCommandQueue(d.dev, &qd, &IID_ID3D12CommandQueue, (void **)&d.queue)) ||
            FAILED(hr = ID3D12Device_CreateCommandAllocator(d.dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
                                                            (void **)&d.alloc)) ||
            FAILED(hr = ID3D12Device_CreateCommandList(d.dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, d.alloc, NULL,
                                                       &IID_ID3D12GraphicsCommandList, (void **)&d.list)) ||
            FAILED(hr = ID3D12Device_CreateFence(d.dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&d.fence)))
            return fail("D3D12 queue, list, fence", hr);
        d.event = CreateEventW(NULL, FALSE, FALSE, NULL);
    } else {
        if (FAILED(hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev11, NULL, &ctx)))
            return fail("D3D11CreateDevice", hr);
        IDXGIDevice *dxgi_dev = NULL;
        IDXGIAdapter *adapter = NULL;
        if (SUCCEEDED(ID3D11Device_QueryInterface(dev11, &IID_IDXGIDevice, (void **)&dxgi_dev)) &&
            SUCCEEDED(IDXGIDevice_GetAdapter(dxgi_dev, &adapter)))
            IDXGIAdapter_GetDesc(adapter, &ad);
    }
    printf("%s adapter: vendor 0x%04x device 0x%04x '%ls'\n", api, ad.VendorId, ad.DeviceId, ad.Description);

    /* 2. NvAPI */
    int nvapi_ok = 0;
    HMODULE nvapi = LoadLibraryA("nvapi64.dll");
    if (nvapi) {
        PFN_nvapi_QueryInterface qi = (PFN_nvapi_QueryInterface)GetProcAddress(nvapi, "nvapi_QueryInterface");
        PFN_NvAPI_Initialize init = qi ? (PFN_NvAPI_Initialize)qi(0x0150e828) : NULL;
        int st = init ? init() : -1;
        printf("nvapi64.dll: NvAPI_Initialize = %d\n", st);
        if (st == 0) {
            PFN_NvAPI_EnumPhysicalGPUs enum_gpus = (PFN_NvAPI_EnumPhysicalGPUs)qi(0xe5ac921f);
            PFN_NvAPI_GPU_GetFullName full_name = (PFN_NvAPI_GPU_GetFullName)qi(0xceee8e9f);
            void *gpus[64] = { 0 };
            unsigned int count = 0;
            char name[64] = "";
            if (enum_gpus && enum_gpus(gpus, &count) == 0 && count > 0 && full_name && full_name(gpus[0], name) == 0) {
                printf("nvapi64.dll: %u GPU(s), '%s'\n", count, name);
                nvapi_ok = 1;
            }
        }
    } else {
        printf("nvapi64.dll: not found (%lu)\n", GetLastError());
    }

    /* 3. NGX core through the registry, like the NGX loader in the game */
    WCHAR full_path[MAX_PATH] = L"", core_path[MAX_PATH + 16];
    DWORD size = sizeof(full_path);
    LSTATUS ls = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"FullPath",
                              RRF_RT_REG_SZ, NULL, full_path, &size);
    HMODULE core = NULL;
    if (ls == ERROR_SUCCESS) {
        swprintf(core_path, MAX_PATH + 16, L"%ls\\_nvngx.dll", full_path);
        core = LoadLibraryW(core_path);
        DWORD override = 0, osize = sizeof(override);
        RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"EnableSignatureOverride",
                     RRF_RT_REG_DWORD, NULL, &override, &osize);
        printf("NGXCore FullPath '%ls', EnableSignatureOverride %lu, _nvngx.dll %s\n", full_path, override,
               core ? "loaded" : "not loadable");
    } else {
        printf("NGXCore FullPath: not set (%ld)\n", (long)ls);
    }

    if (o.expect_off) {
        int off = ad.VendorId != 0x10de && !nvapi_ok && !core;
        printf(off ? "PASS no NVIDIA GPU, NvAPI or NGX visible\n" : "FAIL NVIDIA parts visible without NEUTRON_DLSS\n");
        return off ? 0 : 2;
    }
    if (ad.VendorId != 0x10de) return fail("adapter vendor is not NVIDIA", ad.VendorId);
    if (!nvapi_ok) return fail("NvAPI", 0);
    if (!core) return fail("NGX core", GetLastError());

    char name[96];
#define NGX_PROC(type, fn) (snprintf(name, sizeof(name), "NVSDK_NGX_%s_%s", api, fn), (type)GetProcAddress(core, name))
    PFN_Init_with_ProjectID ngx_init = NGX_PROC(PFN_Init_with_ProjectID, "Init_with_ProjectID");
    PFN_GetParams get_caps = NGX_PROC(PFN_GetParams, "GetCapabilityParameters");
    PFN_GetParams alloc_params = NGX_PROC(PFN_GetParams, "AllocateParameters");
    PFN_CreateFeature create = NGX_PROC(PFN_CreateFeature, "CreateFeature");
    PFN_EvaluateFeature evaluate = NGX_PROC(PFN_EvaluateFeature, "EvaluateFeature");
    PFN_ReleaseFeature release = NGX_PROC(PFN_ReleaseFeature, "ReleaseFeature");
    PFN_DestroyParameters destroy = NGX_PROC(PFN_DestroyParameters, "DestroyParameters");
    PFN_Shutdown1 shutdown = NGX_PROC(PFN_Shutdown1, "Shutdown1");
    if (!ngx_init || !get_caps || !alloc_params || !create || !evaluate || !release || !destroy || !shutdown)
        return fail("NGX core exports", 0);
    void *device = o.d3d12 ? (void *)d.dev : (void *)dev11;
    void *context = o.d3d12 ? (void *)d.list : (void *)ctx;

    unsigned int r = ngx_init("a0f57b54-1daf-4934-90ae-c4035c19df04", 0, "1.0", L"C:\\users\\steamuser\\Temp", device, NULL, 0x15);
    if (!NGX_SUCCEED(r)) return fail("Init_with_ProjectID", r);

    NGXParam *caps = NULL;
    if (!NGX_SUCCEED(r = get_caps(&caps)) || !caps) return fail("GetCapabilityParameters", r);
    unsigned int available = 0;
    int needs_update = 1;
    caps->v->GetUI(caps, "SuperSampling.Available", &available);
    caps->v->GetI(caps, "SuperSampling.NeedsUpdatedDriver", &needs_update);
    printf("NGX: SuperSampling.Available %u, NeedsUpdatedDriver %d\n", available, needs_update);
    if (!available || needs_update) return fail("DLSS not available", available);

    /* NGX_DLSS_GET_OPTIMAL_SETTINGS */
    PFN_OptimalSettings optimal = NULL;
    caps->v->GetVoidPointer(caps, "DLSSOptimalSettingsCallback", (void **)&optimal);
    if (!optimal) return fail("DLSSOptimalSettingsCallback", 0);
    caps->v->SetUI(caps, "Width", DISPLAY_W);
    caps->v->SetUI(caps, "Height", DISPLAY_H);
    caps->v->SetI(caps, "PerfQualityValue", NGX_PERF_MAXPERF);
    caps->v->SetI(caps, "RTXValue", 0);
    if (!NGX_SUCCEED(r = optimal(caps))) return fail("optimal settings callback", r);
    unsigned int rw = 0, rh = 0;
    float sharpness = -1;
    caps->v->GetUI(caps, "OutWidth", &rw);
    caps->v->GetUI(caps, "OutHeight", &rh);
    caps->v->GetF(caps, "Sharpness", &sharpness);
    printf("NGX: performance mode renders %ux%u for %dx%d (sharpness %.2f)\n", rw, rh, DISPLAY_W, DISPLAY_H, sharpness);
    if (rw != DISPLAY_W / 2 || rh != DISPLAY_H / 2) return fail("render size for performance mode", rw);

    g_depth_data = malloc(sizeof(float) * rw * rh);
    g_mv_data = malloc(sizeof(unsigned short) * 2 * rw * rh);
    g_frame = malloc(sizeof(unsigned short) * 4 * rw * rh);
    float *out_img = malloc(sizeof(float) * 3 * DISPLAY_W * DISPLAY_H);
    float *truth = malloc(sizeof(float) * 3 * DISPLAY_W * DISPLAY_H);
    float *bilin = malloc(sizeof(float) * 3 * DISPLAY_W * DISPLAY_H);
    for (unsigned i = 0; i < rw * rh; i++) g_depth_data[i] = 0.5f;

    /* textures as a game would make them */
    ID3D11Texture2D *color11 = NULL, *depth11 = NULL, *mv11 = NULL, *output11 = NULL;
    ID3D12Resource *color12 = NULL, *depth12 = NULL, *mv12 = NULL, *output12 = NULL;
    const UINT color_pitch = ALIGN(rw * 8, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const UINT small_pitch = ALIGN(rw * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const UINT out_pitch = ALIGN(DISPLAY_W * 8, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const UINT64 mv_offset = ALIGN((UINT64)color_pitch * rh, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    const UINT64 depth_offset = ALIGN(mv_offset + (UINT64)small_pitch * rh, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    if (o.d3d12) {
        color12 = d12_texture(&d, rw, rh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        depth12 = d12_texture(&d, rw, rh, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        mv12 = d12_texture(&d, rw, rh, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        output12 = d12_texture(&d, DISPLAY_W, DISPLAY_H, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        d.upload = d12_buffer(&d, D3D12_HEAP_TYPE_UPLOAD, depth_offset + (UINT64)small_pitch * rh);
        d.readback = d12_buffer(&d, D3D12_HEAP_TYPE_READBACK, (UINT64)out_pitch * DISPLAY_H);
        if (!color12 || !depth12 || !mv12 || !output12 || !d.upload || !d.readback) return fail("D3D12 resources", 0);
        D3D12_RANGE none = { 0, 0 };
        if (FAILED(hr = ID3D12Resource_Map(d.upload, 0, &none, (void **)&d.upload_ptr))) return fail("Map(upload)", hr);
        for (unsigned y = 0; y < rh; y++) memcpy(d.upload_ptr + depth_offset + y * small_pitch, g_depth_data + y * rw, rw * 4);
        d12_copy(&d, d.upload, depth_offset, small_pitch, DXGI_FORMAT_R32_FLOAT, rw, rh, depth12, 1);
    } else {
        color11 = make_texture(dev11, rw, rh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
        depth11 = make_texture(dev11, rw, rh, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
        mv11 = make_texture(dev11, rw, rh, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
        output11 = make_texture(dev11, DISPLAY_W, DISPLAY_H, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
        if (!color11 || !depth11 || !mv11 || !output11) return fail("textures", 0);
        ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)depth11, 0, NULL, g_depth_data, rw * 4, 0);
    }

    /* CreateFeature (NGX_D3D11_CREATE_DLSS_EXT / NGX_D3D12_CREATE_DLSS_EXT) */
    NGXParam *p = NULL;
    if (!NGX_SUCCEED(r = alloc_params(&p)) || !p) return fail("AllocateParameters", r);
    p->v->SetUI(p, "Width", rw);
    p->v->SetUI(p, "Height", rh);
    p->v->SetUI(p, "OutWidth", DISPLAY_W);
    p->v->SetUI(p, "OutHeight", DISPLAY_H);
    p->v->SetI(p, "PerfQualityValue", NGX_PERF_MAXPERF);
    p->v->SetI(p, "DLSS.Feature.Create.Flags", NGX_FLAG_MV_LOWRES | (o.noae ? 0 : NGX_FLAG_AUTO_EXPOSURE));
    p->v->SetI(p, "DLSS.Enable.Output.Subrects", 0);
    if (o.d3d12) {
        p->v->SetUI(p, "CreationNodeMask", 1);
        p->v->SetUI(p, "VisibilityNodeMask", 1);
    }
    NGXHandle *handle = NULL;
    if (!NGX_SUCCEED(r = create(context, NGX_FEATURE_SUPERSAMPLING, p, &handle)) || !handle) return fail("CreateFeature(DLSS)", r);
    printf("NGX %s: DLSS feature created, %ux%u -> %dx%d\n", api, rw, rh, DISPLAY_W, DISPLAY_H);

    /* Evaluate (NGX_D3D1x_EVALUATE_DLSS_EXT): phase 1 still, phase 2 scrolling */
    const int still_frames = 32, moving_frames = 32;
    const double vx = 1.5, vy = 0.75;   /* display pixels per frame in phase 2 */
    double err_still = -1, err_moving = -1, ox = 0, oy = 0;
    for (int f = 0; f < still_frames + moving_frames; f++) {
        int moving = f >= still_frames;
        int readback = f == still_frames - 1 || f == still_frames + moving_frames - 1;
        double jx = halton(f % 16 + 1, 2) - 0.5, jy = halton(f % 16 + 1, 3) - 0.5;
        if (moving) { ox += vx; oy += vy; }
        render_frame(g_frame, rw, rh, jx, jy, ox, oy);
        /* motion vectors in render pixels, from the current to the previous position */
        unsigned short mvx = f2h((float)((moving ? -vx : 0) * rw / DISPLAY_W * (o.mvneg ? -1 : 1)));
        unsigned short mvy = f2h((float)((moving ? -vy : 0) * rh / DISPLAY_H * (o.mvneg ? -1 : 1)));
        for (unsigned i = 0; i < rw * rh; i++) { g_mv_data[i * 2] = mvx; g_mv_data[i * 2 + 1] = mvy; }

        if (o.d3d12) {
            for (unsigned y = 0; y < rh; y++) {
                memcpy(d.upload_ptr + y * color_pitch, g_frame + y * rw * 4, rw * 8);
                memcpy(d.upload_ptr + mv_offset + y * small_pitch, g_mv_data + y * rw * 2, rw * 4);
            }
            d12_copy(&d, d.upload, 0, color_pitch, DXGI_FORMAT_R16G16B16A16_FLOAT, rw, rh, color12, 1);
            d12_copy(&d, d.upload, mv_offset, small_pitch, DXGI_FORMAT_R16G16_FLOAT, rw, rh, mv12, 1);
            d12_barrier(&d, color12, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            d12_barrier(&d, mv12, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            p->v->SetD3d12Resource(p, "Color", color12);
            p->v->SetD3d12Resource(p, "Output", output12);
            p->v->SetD3d12Resource(p, "Depth", depth12);
            p->v->SetD3d12Resource(p, "MotionVectors", mv12);
            p->v->SetD3d12Resource(p, "ExposureTexture", NULL);
        } else {
            ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)color11, 0, NULL, g_frame, rw * 8, 0);
            ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)mv11, 0, NULL, g_mv_data, rw * 4, 0);
            p->v->SetD3d11Resource(p, "Color", (ID3D11Resource *)color11);
            p->v->SetD3d11Resource(p, "Output", (ID3D11Resource *)output11);
            p->v->SetD3d11Resource(p, "Depth", (ID3D11Resource *)depth11);
            p->v->SetD3d11Resource(p, "MotionVectors", (ID3D11Resource *)mv11);
            p->v->SetD3d11Resource(p, "ExposureTexture", NULL);
        }
        p->v->SetF(p, "Jitter.Offset.X", (float)(o.jneg ? -jx : jx));
        p->v->SetF(p, "Jitter.Offset.Y", (float)(o.jneg ? -jy : jy));
        p->v->SetF(p, "Sharpness", 0.0f);
        p->v->SetI(p, "Reset", f == 0);
        p->v->SetF(p, "MV.Scale.X", 1.0f);
        p->v->SetF(p, "MV.Scale.Y", 1.0f);
        p->v->SetUI(p, "DLSS.Input.Color.Subrect.Base.X", 0);
        p->v->SetUI(p, "DLSS.Input.Color.Subrect.Base.Y", 0);
        p->v->SetUI(p, "DLSS.Render.Subrect.Dimensions.Width", o.nosubrect ? 0 : rw);
        p->v->SetUI(p, "DLSS.Render.Subrect.Dimensions.Height", o.nosubrect ? 0 : rh);
        p->v->SetF(p, "DLSS.Pre.Exposure", 1.0f);
        p->v->SetF(p, "DLSS.Exposure.Scale", 1.0f);
        if (!o.noeval && !NGX_SUCCEED(r = evaluate(context, handle, p, NULL))) return fail("EvaluateFeature(DLSS)", r);

        /* one frame per Metal command buffer, like a game with Present: the MetalFX temporal
           scaler times out with more than about 20 encodes in one command buffer */
        if (o.d3d12) {
            d12_barrier(&d, color12, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            d12_barrier(&d, mv12, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            if (readback) d12_copy(&d, d.readback, 0, out_pitch, DXGI_FORMAT_R16G16B16A16_FLOAT, DISPLAY_W, DISPLAY_H, output12, 0);
            if (!d12_submit(&d)) return fail("ExecuteCommandLists / fence wait", 0);
        } else {
            ID3D11DeviceContext_Flush(ctx);
        }

        if (readback) {
            if (o.d3d12) {
                unsigned char *rb = NULL;
                D3D12_RANGE all = { 0, (SIZE_T)out_pitch * DISPLAY_H };
                if (FAILED(hr = ID3D12Resource_Map(d.readback, 0, &all, (void **)&rb))) return fail("Map(readback)", hr);
                for (int y = 0; y < DISPLAY_H; y++) {
                    const unsigned short *row = (const unsigned short *)(rb + y * out_pitch);
                    for (int x = 0; x < DISPLAY_W; x++)
                        for (int c = 0; c < 3; c++) out_img[(y * DISPLAY_W + x) * 3 + c] = h2f(row[x * 4 + c]);
                }
                D3D12_RANGE none = { 0, 0 };
                ID3D12Resource_Unmap(d.readback, 0, &none);
            } else if (!read_output(dev11, ctx, output11, out_img)) {
                return fail("readback", 0);
            }
            truth_image(truth, ox, oy);
            double e = image_error(out_img, truth);
            if (moving) {
                err_moving = e;
                dump_ppm("moving", out_img);
            } else {
                err_still = e;
                dump_ppm("out", out_img);
                dump_ppm("truth", truth);
            }
        }
    }

    /* plausibility of the last output: mean close to the exact picture's mean, not flat */
    double mean = 0, var = 0;
    for (int i = 0; i < DISPLAY_W * DISPLAY_H * 3; i++) mean += out_img[i];
    mean /= DISPLAY_W * DISPLAY_H * 3;
    for (int i = 0; i < DISPLAY_W * DISPLAY_H * 3; i++) var += (out_img[i] - mean) * (out_img[i] - mean);
    var /= DISPLAY_W * DISPLAY_H * 3;

    /* reference: one frame without jitter, bilinear to display size */
    render_frame(g_frame, rw, rh, 0, 0, 0, 0);
    bilinear(g_frame, rw, rh, bilin);
    truth_image(truth, 0, 0);
    double err_bilinear = image_error(bilin, truth);
    dump_ppm("bilinear", bilin);

    printf("error vs exact picture (mean abs, 0..255): bilinear %.2f, DLSS still %.2f, DLSS moving %.2f\n",
           err_bilinear, err_still, err_moving);
    printf("output mean %.3f, stddev %.3f\n", mean, sqrt(var));

    release(handle);
    destroy(p);
    shutdown(device);

    int ok = err_still >= 0 && err_still < err_bilinear * 0.85 && err_moving >= 0 && err_moving < err_bilinear &&
             sqrt(var) > 0.1 && mean > 0.2 && mean < 0.8;
    if (ok) printf("PASS DLSS (%s) on MetalFX temporal: upscaled output closer to the exact picture than bilinear\n", api);
    else printf("FAIL DLSS (%s) output not plausible\n", api);
    return ok ? 0 : 2;
}
