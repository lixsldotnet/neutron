/* neutron headless test: D3D12 queries through DXMT. Occlusion counts (one draw, two draws, a
 * scissored-out draw), binary occlusion (drawn and empty), timestamps across two submissions with
 * a sleep in between (order and GetTimestampFrequency), and pipeline statistics, which Metal does
 * not have and which resolve to zeros. Results go to a readback buffer with ResolveQueryData.
 * Exit code 0 = all values match.
 * Build: x86_64-w64-mingw32-clang -O2 -o d3d12_queries.exe d3d12_queries.c -ld3d12 -ld3dcompiler
 */
#define COBJMACROS
#define INITGUID
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 64

static ID3D12Device *dev;
static ID3D12CommandQueue *queue;
static ID3D12CommandAllocator *alloc;
static ID3D12GraphicsCommandList *list;
static ID3D12Fence *fence;
static UINT64 fence_value;
static HANDLE fence_event;
static int failures;

static const char shader_src[] =
    "float4 vs(uint id : SV_VertexID) : SV_Position\n"
    "{\n"
    "    float2 uv = float2((id << 1) & 2, id & 2);\n"
    "    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "}\n"
    "float4 ps() : SV_Target { return float4(1, 1, 1, 1); }\n";

static ID3DBlob *compile(const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *errors = NULL;
    if (FAILED(D3DCompile(shader_src, sizeof(shader_src) - 1, "q", NULL, NULL, entry, target, 0, 0, &code, &errors)))
    {
        printf("compile %s failed\n", entry);
        exit(1);
    }
    return code;
}

static void check(const char *name, UINT64 got, UINT64 want)
{
    if (got != want)
    {
        printf("FAIL %s: %llu, expected %llu\n", name, (unsigned long long)got, (unsigned long long)want);
        failures++;
    }
}

static void submit_and_wait(void)
{
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
    if (FAILED(ID3D12GraphicsCommandList_Close(list)))
    {
        printf("FAIL Close\n");
        exit(1);
    }
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    ID3D12CommandQueue_Signal(queue, fence, ++fence_value);
    ID3D12Fence_SetEventOnCompletion(fence, fence_value, fence_event);
    if (WaitForSingleObject(fence_event, 10000) != WAIT_OBJECT_0)
    {
        printf("FAIL GPU wait timed out\n");
        exit(1);
    }
    ID3D12CommandAllocator_Reset(alloc);
    ID3D12GraphicsCommandList_Reset(list, alloc, NULL);
}

static ID3D12QueryHeap *make_heap(D3D12_QUERY_HEAP_TYPE type, UINT count)
{
    D3D12_QUERY_HEAP_DESC qd = { type, count, 0 };
    ID3D12QueryHeap *heap;
    HRESULT hr = ID3D12Device_CreateQueryHeap(dev, &qd, &IID_ID3D12QueryHeap, (void **)&heap);
    if (FAILED(hr))
    {
        printf("FAIL CreateQueryHeap(%u): hr=0x%08lx\n", type, (unsigned long)hr);
        exit(1);
    }
    return heap;
}

int main(void)
{
    ID3D12RootSignature *root;
    ID3D12PipelineState *pso;
    ID3D12Resource *target, *readback;
    ID3D12DescriptorHeap *rtv_heap;
    ID3DBlob *blob = NULL, *err = NULL;
    HRESULT hr;

    if (FAILED(hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev)))
    {
        printf("FAIL D3D12CreateDevice: hr=0x%08lx\n", (unsigned long)hr);
        return 1;
    }
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue);
    ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
                                        (void **)&alloc);
    ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList,
                                   (void **)&list);
    ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);
    fence_event = CreateEventA(NULL, FALSE, FALSE, NULL);

    D3D12_ROOT_SIGNATURE_DESC rsd = { 0 };
    D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
                                     &IID_ID3D12RootSignature, (void **)&root);
    ID3DBlob *vs = compile("vs", "vs_5_0"), *ps = compile("ps", "ps_5_0");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = { 0 };
    pd.pRootSignature = root;
    pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vs);
    pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vs);
    pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(ps);
    pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(ps);
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = 0xffffffff;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    if (FAILED(hr = ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso)))
    {
        printf("FAIL CreateGraphicsPipelineState: hr=0x%08lx\n", (unsigned long)hr);
        return 1;
    }

    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = { 0 };
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = W;
    td.Height = H;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL,
                                         &IID_ID3D12Resource, (void **)&target);
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap);
    ID3D12Device_CreateRenderTargetView(dev, target, NULL, rtv);

    D3D12_HEAP_PROPERTIES rp = { D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC bd = { 0 };
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 4096;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Device_CreateCommittedResource(dev, &rp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                         &IID_ID3D12Resource, (void **)&readback);
    UINT64 *out;
    ID3D12Resource_Map(readback, 0, NULL, (void **)&out);
    memset(out, 0xff, 4096);

    ID3D12QueryHeap *occ = make_heap(D3D12_QUERY_HEAP_TYPE_OCCLUSION, 8);
    ID3D12QueryHeap *ts = make_heap(D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 4);
    ID3D12QueryHeap *stats = make_heap(D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, 2);

    const float black[4] = { 0, 0, 0, 1 };
    D3D12_VIEWPORT vp = { 0, 0, W, H, 0, 1 };
    D3D12_RECT left = { 0, 0, W / 2, H }, none = { 0, 0, 0, 0 };

    ID3D12GraphicsCommandList_EndQuery(list, ts, D3D12_QUERY_TYPE_TIMESTAMP, 0);
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, black, 0, NULL);
    ID3D12GraphicsCommandList_SetPipelineState(list, pso);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, root);
    ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &left);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    /* 0: one draw over the left half = 32 * 64 samples */
    ID3D12GraphicsCommandList_BeginQuery(list, occ, D3D12_QUERY_TYPE_OCCLUSION, 0);
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
    ID3D12GraphicsCommandList_EndQuery(list, occ, D3D12_QUERY_TYPE_OCCLUSION, 0);
    /* 1: binary, same draw */
    ID3D12GraphicsCommandList_BeginQuery(list, occ, D3D12_QUERY_TYPE_BINARY_OCCLUSION, 1);
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
    ID3D12GraphicsCommandList_EndQuery(list, occ, D3D12_QUERY_TYPE_BINARY_OCCLUSION, 1);
    /* 2: two draws in one query */
    ID3D12GraphicsCommandList_BeginQuery(list, occ, D3D12_QUERY_TYPE_OCCLUSION, 2);
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
    ID3D12GraphicsCommandList_EndQuery(list, occ, D3D12_QUERY_TYPE_OCCLUSION, 2);
    /* 3: everything scissored out */
    ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &none);
    ID3D12GraphicsCommandList_BeginQuery(list, occ, D3D12_QUERY_TYPE_OCCLUSION, 3);
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
    ID3D12GraphicsCommandList_EndQuery(list, occ, D3D12_QUERY_TYPE_OCCLUSION, 3);
    /* 4: binary without any draw */
    ID3D12GraphicsCommandList_BeginQuery(list, occ, D3D12_QUERY_TYPE_BINARY_OCCLUSION, 4);
    ID3D12GraphicsCommandList_EndQuery(list, occ, D3D12_QUERY_TYPE_BINARY_OCCLUSION, 4);
    /* 5: begun before the render pass exists */
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, black, 0, NULL);
    ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &left);
    ID3D12GraphicsCommandList_BeginQuery(list, occ, D3D12_QUERY_TYPE_BINARY_OCCLUSION, 5);
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
    ID3D12GraphicsCommandList_EndQuery(list, occ, D3D12_QUERY_TYPE_BINARY_OCCLUSION, 5);
    ID3D12GraphicsCommandList_EndQuery(list, ts, D3D12_QUERY_TYPE_TIMESTAMP, 1);
    ID3D12GraphicsCommandList_ResolveQueryData(list, occ, D3D12_QUERY_TYPE_OCCLUSION, 0, 6, readback, 0);
    ID3D12GraphicsCommandList_ResolveQueryData(list, stats, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 2, readback, 1024);
    submit_and_wait();

    check("occlusion one draw", out[0], 32 * 64);
    check("binary drawn", out[1], 1);
    check("occlusion two draws", out[2], 2 * 32 * 64);
    check("occlusion scissored out", out[3], 0);
    check("binary empty", out[4], 0);
    check("binary begun before the pass", out[5], 1);
    for (unsigned i = 0; i < 2 * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS) / 8; i++)
        check("pipeline statistics", out[128 + i], 0);

    Sleep(100);
    ID3D12GraphicsCommandList_EndQuery(list, ts, D3D12_QUERY_TYPE_TIMESTAMP, 2);
    /* resolved from another command list than the one that wrote 0 and 1 */
    ID3D12GraphicsCommandList_ResolveQueryData(list, ts, D3D12_QUERY_TYPE_TIMESTAMP, 0, 3, readback, 2048);
    submit_and_wait();

    UINT64 freq = 0;
    ID3D12CommandQueue_GetTimestampFrequency(queue, &freq);
    UINT64 t0 = out[256], t1 = out[257], t2 = out[258];
    double gap = freq ? (double)(t2 - t1) / freq : 0;
    printf("timestamps %llu %llu %llu, frequency %llu, gap %.3f s\n", (unsigned long long)t0,
           (unsigned long long)t1, (unsigned long long)t2, (unsigned long long)freq, gap);
    if (!(t0 && t0 <= t1 && t1 < t2) || gap < 0.09 || gap > 5)
    {
        printf("FAIL timestamps out of order or wrong gap\n");
        failures++;
    }

    if (failures)
    {
        printf("FAIL %d query checks\n", failures);
        return 2;
    }
    printf("PASS D3D12 queries\n");
    return 0;
}
