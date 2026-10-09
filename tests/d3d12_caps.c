/* neutron headless test: what Unreal's D3D12 RHI sees at startup.
 * No window, no swapchain. Enumerates DXGI adapters the way UE's FindAdapter
 * does, creates a D3D12 device, queries every CheckFeatureSupport block UE
 * (and other engines) look at and prints the result, then computes UE's
 * MaxRHIFeatureLevel (SM6 / SM5 / none) from the same rules.
 *
 * Build: x86_64-w64-mingw32-clang -O2 -o d3d12_caps.exe d3d12_caps.c -ldxgi -ld3d12 -lole32
 * Run:   tool/neutron runinprefix d3d12_caps.exe
 * Exit code: 0 = UE would accept SM6, 2 = only SM5, 1 = nothing / error. */
#define COBJMACROS
#define INITGUID
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>

static const char *fl_name(D3D_FEATURE_LEVEL fl)
{
    switch (fl) {
    case D3D_FEATURE_LEVEL_11_0: return "11_0";
    case D3D_FEATURE_LEVEL_11_1: return "11_1";
    case D3D_FEATURE_LEVEL_12_0: return "12_0";
    case D3D_FEATURE_LEVEL_12_1: return "12_1";
    case 0xc200: return "12_2";
    default: return "?";
    }
}

#define CFS(feat, var) \
    do { hr = ID3D12Device_CheckFeatureSupport(dev, feat, &var, sizeof(var)); \
         printf("%-34s hr=0x%08lx\n", #feat, (unsigned long)hr); } while (0)

int main(void)
{
    IDXGIFactory4 *f4 = NULL;
    IDXGIFactory6 *f6 = NULL;
    IDXGIAdapter1 *adapter = NULL;
    ID3D12Device *dev = NULL;
    HRESULT hr;
    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    char path[MAX_PATH] = "";

    GetModuleFileNameA(d3d12, path, sizeof(path));
    printf("d3d12.dll: %s\n", path);
    printf("exports: D3D12EnableExperimentalFeatures=%p D3D12GetInterface=%p D3D12SDKVersion(own exe)=%p\n",
           GetProcAddress(d3d12, "D3D12EnableExperimentalFeatures"), GetProcAddress(d3d12, "D3D12GetInterface"),
           GetProcAddress(GetModuleHandleA(NULL), "D3D12SDKVersion"));

    /* --- DXGI, like UE's SafeCreateDXGIFactory + EnumAdapters --- */
    if (FAILED(hr = CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&f4))) {
        printf("FAIL CreateDXGIFactory2(IDXGIFactory4) hr=0x%08lx\n", (unsigned long)hr);
        return 1;
    }
    hr = IDXGIFactory4_QueryInterface(f4, &IID_IDXGIFactory6, (void **)&f6);
    printf("QI IDXGIFactory6 hr=0x%08lx\n", (unsigned long)hr);

    for (UINT i = 0;; i++) {
        IDXGIAdapter1 *a = NULL;
        if (f6)
            hr = IDXGIFactory6_EnumAdapterByGpuPreference(f6, i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                          &IID_IDXGIAdapter1, (void **)&a);
        else
            hr = IDXGIFactory4_EnumAdapters1(f4, i, &a);
        if (hr == DXGI_ERROR_NOT_FOUND)
            break;
        if (FAILED(hr)) {
            printf("adapter %u: enum hr=0x%08lx\n", i, (unsigned long)hr);
            break;
        }
        DXGI_ADAPTER_DESC1 d;
        IDXGIAdapter1_GetDesc1(a, &d);
        printf("adapter %u: \"%ls\" vendor=0x%04x device=0x%04x subsys=0x%08x rev=%u flags=0x%x\n"
               "  dedicatedVideo=%llu MB dedicatedSys=%llu MB sharedSys=%llu MB luid=%08lx:%08lx\n",
               i, d.Description, d.VendorId, d.DeviceId, d.SubSysId, d.Revision, d.Flags,
               (unsigned long long)d.DedicatedVideoMemory >> 20, (unsigned long long)d.DedicatedSystemMemory >> 20,
               (unsigned long long)d.SharedSystemMemory >> 20, d.AdapterLuid.HighPart, d.AdapterLuid.LowPart);
        IDXGIAdapter3 *a3 = NULL;
        if (SUCCEEDED(IDXGIAdapter1_QueryInterface(a, &IID_IDXGIAdapter3, (void **)&a3))) {
            DXGI_QUERY_VIDEO_MEMORY_INFO mi = { 0 };
            hr = IDXGIAdapter3_QueryVideoMemoryInfo(a3, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mi);
            printf("  QueryVideoMemoryInfo(local) hr=0x%08lx budget=%llu MB usage=%llu MB\n", (unsigned long)hr,
                   (unsigned long long)mi.Budget >> 20, (unsigned long long)mi.CurrentUsage >> 20);
            hr = IDXGIAdapter3_QueryVideoMemoryInfo(a3, 0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &mi);
            printf("  QueryVideoMemoryInfo(nonlocal) hr=0x%08lx budget=%llu MB\n", (unsigned long)hr,
                   (unsigned long long)mi.Budget >> 20);
            IDXGIAdapter3_Release(a3);
        } else
            printf("  no IDXGIAdapter3\n");
        IDXGIAdapter4 *a4 = NULL;
        if (SUCCEEDED(IDXGIAdapter1_QueryInterface(a, &IID_IDXGIAdapter4, (void **)&a4))) {
            DXGI_ADAPTER_DESC3 d3;
            hr = IDXGIAdapter4_GetDesc3(a4, &d3);
            printf("  GetDesc3 hr=0x%08lx flags=0x%x preemption gfx=%d compute=%d\n", (unsigned long)hr, d3.Flags,
                   d3.GraphicsPreemptionGranularity, d3.ComputePreemptionGranularity);
            IDXGIAdapter4_Release(a4);
        } else
            printf("  no IDXGIAdapter4\n");
        LARGE_INTEGER umd = { 0 };
        hr = IDXGIAdapter1_CheckInterfaceSupport(a, &IID_IDXGIDevice, &umd);
        printf("  CheckInterfaceSupport(IDXGIDevice) hr=0x%08lx umd=%lld\n", (unsigned long)hr, umd.QuadPart);
        if (!adapter)
            adapter = a;
        else
            IDXGIAdapter1_Release(a);
    }
    if (!adapter) {
        printf("FAIL no adapter\n");
        return 1;
    }

    /* --- debug layer / experimental features --- */
    {
        ID3D12Debug *dbg = NULL;
        hr = D3D12GetDebugInterface(&IID_ID3D12Debug, (void **)&dbg);
        printf("D3D12GetDebugInterface(ID3D12Debug) hr=0x%08lx\n", (unsigned long)hr);
        if (dbg)
            ID3D12Debug_Release(dbg);
        typedef HRESULT (WINAPI *pfn_exp)(UINT, const IID *, void *, UINT *);
        pfn_exp exp = (pfn_exp)GetProcAddress(d3d12, "D3D12EnableExperimentalFeatures");
        /* D3D12ExperimentalShaderModels, what old UE asked for to get SM 6.x before 6.0 shipped */
        static const GUID shader_models = { 0x76f5573e, 0xf13a, 0x40f5, { 0xb2, 0x97, 0x81, 0xce, 0x9e, 0x18, 0x93, 0x3f } };
        if (exp) {
            hr = exp(1, &shader_models, NULL, NULL);
            printf("D3D12EnableExperimentalFeatures(ShaderModels) hr=0x%08lx\n", (unsigned long)hr);
        }
    }

    /* --- D3D12CreateDevice at each level (UE: SafeTestD3D12CreateDevice with the minimum FL) --- */
    static const D3D_FEATURE_LEVEL fls[] = { 0xc200, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
                                             D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    for (unsigned i = 0; i < ARRAYSIZE(fls); i++) {
        hr = D3D12CreateDevice((IUnknown *)adapter, fls[i], &IID_ID3D12Device, NULL);
        printf("D3D12CreateDevice(min %s, NULL out) hr=0x%08lx\n", fl_name(fls[i]), (unsigned long)hr);
    }
    if (FAILED(hr = D3D12CreateDevice((IUnknown *)adapter, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev))) {
        printf("FAIL D3D12CreateDevice(11_0) hr=0x%08lx\n", (unsigned long)hr);
        return 1;
    }
    LUID luid = ID3D12Device_GetAdapterLuid(dev);
    printf("device ok, nodes=%u luid=%08lx:%08lx\n", ID3D12Device_GetNodeCount(dev), luid.HighPart, luid.LowPart);

    static const struct { const IID *iid; const char *name; } ifaces[] = {
        { &IID_ID3D12Device1, "ID3D12Device1" }, { &IID_ID3D12Device2, "ID3D12Device2" },
        { &IID_ID3D12Device3, "ID3D12Device3" }, { &IID_ID3D12Device4, "ID3D12Device4" },
        { &IID_ID3D12Device5, "ID3D12Device5" }, { &IID_ID3D12Device6, "ID3D12Device6" },
        { &IID_ID3D12Device7, "ID3D12Device7" }, { &IID_ID3D12Device8, "ID3D12Device8" },
        { &IID_ID3D12Device9, "ID3D12Device9" }, { &IID_ID3D12InfoQueue, "ID3D12InfoQueue" },
    };
    for (unsigned i = 0; i < ARRAYSIZE(ifaces); i++) {
        IUnknown *u = NULL;
        hr = ID3D12Device_QueryInterface(dev, ifaces[i].iid, (void **)&u);
        printf("QI %-16s hr=0x%08lx\n", ifaces[i].name, (unsigned long)hr);
        if (u)
            IUnknown_Release(u);
    }

    /* --- feature queries --- */
    D3D12_FEATURE_DATA_FEATURE_LEVELS fl = { ARRAYSIZE(fls), fls };
    CFS(D3D12_FEATURE_FEATURE_LEVELS, fl);
    printf("  MaxSupportedFeatureLevel=%s\n", fl_name(fl.MaxSupportedFeatureLevel));

    /* UE FindHighestShaderModel: ask from the top, first success wins */
    static const D3D_SHADER_MODEL sms[] = { D3D_SHADER_MODEL_6_7, D3D_SHADER_MODEL_6_6, D3D_SHADER_MODEL_6_5,
        D3D_SHADER_MODEL_6_4, D3D_SHADER_MODEL_6_3, D3D_SHADER_MODEL_6_2, D3D_SHADER_MODEL_6_1,
        D3D_SHADER_MODEL_6_0, D3D_SHADER_MODEL_5_1 };
    D3D_SHADER_MODEL max_sm = D3D_SHADER_MODEL_5_1;
    for (unsigned i = 0; i < ARRAYSIZE(sms); i++) {
        D3D12_FEATURE_DATA_SHADER_MODEL sm = { sms[i] };
        hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm));
        printf("SHADER_MODEL ask 0x%x -> hr=0x%08lx HighestShaderModel=0x%x\n", sms[i], (unsigned long)hr,
               sm.HighestShaderModel);
        if (SUCCEEDED(hr)) {
            max_sm = sm.HighestShaderModel;
            break;
        }
    }

    D3D12_FEATURE_DATA_ROOT_SIGNATURE rs = { D3D_ROOT_SIGNATURE_VERSION_1_1 };
    CFS(D3D12_FEATURE_ROOT_SIGNATURE, rs);
    printf("  HighestVersion=0x%x\n", rs.HighestVersion);

    D3D12_FEATURE_DATA_D3D12_OPTIONS o = { 0 };
    CFS(D3D12_FEATURE_D3D12_OPTIONS, o);
    printf("  ResourceBindingTier=%d ResourceHeapTier=%d TiledResourcesTier=%d TypedUAVLoadAdditionalFormats=%d\n"
           "  ROVs=%d ConservativeRaster=%d DoublePrecision=%d MinPrecision=%d PSStencilRef=%d VPRTFromAnyShader=%d\n",
           o.ResourceBindingTier, o.ResourceHeapTier, o.TiledResourcesTier, o.TypedUAVLoadAdditionalFormats,
           o.ROVsSupported, o.ConservativeRasterizationTier, o.DoublePrecisionFloatShaderOps,
           o.MinPrecisionSupport, o.PSSpecifiedStencilRefSupported,
           o.VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation);

    D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1 = { 0 };
    CFS(D3D12_FEATURE_D3D12_OPTIONS1, o1);
    printf("  WaveOps=%d WaveLaneCountMin=%u Max=%u Int64ShaderOps=%d\n", o1.WaveOps, o1.WaveLaneCountMin,
           o1.WaveLaneCountMax, o1.Int64ShaderOps);
    D3D12_FEATURE_DATA_D3D12_OPTIONS2 o2 = { 0 };
    CFS(D3D12_FEATURE_D3D12_OPTIONS2, o2);
    printf("  DepthBoundsTest=%d\n", o2.DepthBoundsTestSupported);
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3 = { 0 };
    CFS(D3D12_FEATURE_D3D12_OPTIONS3, o3);
    printf("  CopyQueueTimestamps=%d Barycentrics=%d\n", o3.CopyQueueTimestampQueriesSupported, o3.BarycentricsSupported);
    D3D12_FEATURE_DATA_D3D12_OPTIONS4 o4 = { 0 };
    CFS(D3D12_FEATURE_D3D12_OPTIONS4, o4);
    printf("  Native16BitShaderOps=%d\n", o4.Native16BitShaderOpsSupported);
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = { 0 };
    CFS(D3D12_FEATURE_D3D12_OPTIONS5, o5);
    printf("  RaytracingTier=%d RenderPassesTier=%d SRVOnlyTiledTier3=%d\n", o5.RaytracingTier, o5.RenderPassesTier,
           o5.SRVOnlyTiledResourceTier3);
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6 = { 0 };
    CFS(D3D12_FEATURE_D3D12_OPTIONS6, o6);
    printf("  VariableShadingRateTier=%d\n", o6.VariableShadingRateTier);
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7 = { 0 };
    CFS(D3D12_FEATURE_D3D12_OPTIONS7, o7);
    printf("  MeshShaderTier=%d SamplerFeedbackTier=%d\n", o7.MeshShaderTier, o7.SamplerFeedbackTier);
    D3D12_FEATURE_DATA_D3D12_OPTIONS8 o8 = { 0 };
    CFS(D3D12_FEATURE_D3D12_OPTIONS8, o8);
    D3D12_FEATURE_DATA_D3D12_OPTIONS9 o9 = { 0 };
    CFS(D3D12_FEATURE_D3D12_OPTIONS9, o9);
    printf("  AtomicInt64OnTypedResource=%d AtomicInt64OnGroupShared=%d WaveMMATier=%d\n",
           o9.AtomicInt64OnTypedResourceSupported, o9.AtomicInt64OnGroupSharedSupported, o9.WaveMMATier);
    D3D12_FEATURE_DATA_D3D12_OPTIONS10 o10 = { 0 };
    CFS(D3D12_FEATURE_D3D12_OPTIONS10, o10);
    D3D12_FEATURE_DATA_D3D12_OPTIONS11 o11 = { 0 };
    CFS(D3D12_FEATURE_D3D12_OPTIONS11, o11);
    printf("  AtomicInt64OnDescriptorHeapResource=%d\n", o11.AtomicInt64OnDescriptorHeapResourceSupported);
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12 = { 0 };
    CFS(D3D12_FEATURE_D3D12_OPTIONS12, o12);
    printf("  EnhancedBarriers=%d\n", o12.EnhancedBarriersSupported);
    D3D12_FEATURE_DATA_D3D12_OPTIONS13 o13 = { 0 };
    CFS(D3D12_FEATURE_D3D12_OPTIONS13, o13);
    D3D12_FEATURE_DATA_D3D12_OPTIONS14 o14 = { 0 };
    CFS(D3D12_FEATURE_D3D12_OPTIONS14, o14);
    D3D12_FEATURE_DATA_D3D12_OPTIONS15 o15 = { 0 };
    CFS(D3D12_FEATURE_D3D12_OPTIONS15, o15);
    D3D12_FEATURE_DATA_D3D12_OPTIONS16 o16 = { 0 };
    CFS(D3D12_FEATURE_D3D12_OPTIONS16, o16);
    D3D12_FEATURE_DATA_ARCHITECTURE1 arch = { 0 };
    CFS(D3D12_FEATURE_ARCHITECTURE1, arch);
    printf("  UMA=%d CacheCoherentUMA=%d TileBased=%d\n", arch.UMA, arch.CacheCoherentUMA, arch.TileBasedRenderer);
    D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT va = { 0 };
    CFS(D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT, va);
    D3D12_FEATURE_DATA_SHADER_CACHE sc = { 0 };
    CFS(D3D12_FEATURE_SHADER_CACHE, sc);
    D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY qp = { D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_QUEUE_PRIORITY_HIGH };
    CFS(D3D12_FEATURE_COMMAND_QUEUE_PRIORITY, qp);
    printf("  PriorityForTypeIsSupported=%d\n", qp.PriorityForTypeIsSupported);
    D3D12_FEATURE_DATA_EXISTING_HEAPS eh = { 0 };
    CFS(D3D12_FEATURE_EXISTING_HEAPS, eh);
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = { DXGI_FORMAT_R32_UINT };
    CFS(D3D12_FEATURE_FORMAT_SUPPORT, fs);
    printf("  R32_UINT Support1=0x%x Support2=0x%x\n", fs.Support1, fs.Support2);

    /* versioned root signature 1.1, UE serializes with this */
    {
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC vd = { D3D_ROOT_SIGNATURE_VERSION_1_1 };
        ID3DBlob *blob = NULL, *err = NULL;
        hr = D3D12SerializeVersionedRootSignature(&vd, &blob, &err);
        printf("D3D12SerializeVersionedRootSignature(1.1, empty) hr=0x%08lx\n", (unsigned long)hr);
        if (SUCCEEDED(hr)) {
            ID3D12RootSignature *rsig = NULL;
            hr = ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob),
                                                  ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature,
                                                  (void **)&rsig);
            printf("CreateRootSignature hr=0x%08lx\n", (unsigned long)hr);
            if (rsig)
                ID3D12RootSignature_Release(rsig);
            ID3D10Blob_Release(blob);
        }
    }

    /* --- UE5 FindMaxRHIFeatureLevel --- */
    int sm6 = fl.MaxSupportedFeatureLevel >= D3D_FEATURE_LEVEL_12_0 && max_sm >= D3D_SHADER_MODEL_6_6 &&
              o.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_3 && o.ResourceHeapTier >= D3D12_RESOURCE_HEAP_TIER_2 &&
              o1.WaveOps && o9.AtomicInt64OnTypedResourceSupported;
    int sm5 = fl.MaxSupportedFeatureLevel >= D3D_FEATURE_LEVEL_11_0 && max_sm >= D3D_SHADER_MODEL_5_1;
    printf("\nUE5 view: FL=%s SM=0x%x BindingTier=%d HeapTier=%d WaveOps=%d Atomic64=%d\n",
           fl_name(fl.MaxSupportedFeatureLevel), max_sm, o.ResourceBindingTier, o.ResourceHeapTier, o1.WaveOps,
           o9.AtomicInt64OnTypedResourceSupported);
    printf("UE5 MaxRHIFeatureLevel: %s\n", sm6 ? "SM6" : sm5 ? "SM5 (D3D12 refused if the project ships only SM6 for D3D12)" : "none");
    if (!sm6) {
        printf("missing for SM6:%s%s%s%s%s%s\n", fl.MaxSupportedFeatureLevel < D3D_FEATURE_LEVEL_12_0 ? " FL12_0" : "",
               max_sm < D3D_SHADER_MODEL_6_6 ? " SM6.6" : "",
               o.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_3 ? " BindingTier3" : "",
               o.ResourceHeapTier < D3D12_RESOURCE_HEAP_TIER_2 ? " HeapTier2" : "", o1.WaveOps ? "" : " WaveOps",
               o9.AtomicInt64OnTypedResourceSupported ? "" : " Atomic64");
    }

    ID3D12Device_Release(dev);
    IDXGIAdapter1_Release(adapter);
    return sm6 ? 0 : sm5 ? 2 : 1;
}
