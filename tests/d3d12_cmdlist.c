/* neutron headless test: D3D12 command list methods that used to abort in DXMT. Buffer to buffer
 * CopyTextureRegion (placed footprints with a box and an offset), WriteBufferImmediate,
 * ResolveSubresourceRegion (whole region, and DECOMPRESS as a no-op), ExecuteBundle (state set
 * in the bundle stays set), and DiscardResource on a render target (in and after a render pass,
 * before a clear, before a full-screen draw; another target keeps its contents).
 * Exit code 0 = all values match.
 * Build: x86_64-w64-mingw32-clang -O2 -o d3d12_cmdlist.exe d3d12_cmdlist.c -ld3d12 -ld3dcompiler
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

#define W 16
#define H 16

static ID3D12Device *dev;
static ID3D12CommandQueue *queue;
static ID3D12CommandAllocator *alloc;
static ID3D12GraphicsCommandList *list;
static ID3D12Fence *fence;
static UINT64 fence_value;
static HANDLE fence_event;
static int failures;

static const char shader_src[] =
    "cbuffer C : register(b0) { float4 color; };\n"
    "float4 vs(uint id : SV_VertexID) : SV_Position\n"
    "{\n"
    "    float2 uv = float2((id << 1) & 2, id & 2);\n"
    "    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "}\n"
    "float4 ps() : SV_Target { return color; }\n";

static ID3DBlob *compile(const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *errors = NULL;
    if (FAILED(D3DCompile(shader_src, sizeof(shader_src) - 1, "c", NULL, NULL, entry, target, 0, 0, &code, &errors)))
    {
        printf("compile %s failed\n", entry);
        exit(1);
    }
    return code;
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

static ID3D12Resource *make_buffer(D3D12_HEAP_TYPE type, UINT64 size)
{
    D3D12_HEAP_PROPERTIES hp = { type };
    D3D12_RESOURCE_DESC bd = { 0 };
    ID3D12Resource *res;
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = size;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &bd,
            type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void **)&res)))
    {
        printf("FAIL CreateCommittedResource(buffer)\n");
        exit(1);
    }
    return res;
}

static ID3D12Resource *make_target(UINT samples, D3D12_CPU_DESCRIPTOR_HANDLE rtv)
{
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = { 0 };
    ID3D12Resource *res;
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = W;
    td.Height = H;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = samples;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (FAILED(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&res)))
    {
        printf("FAIL CreateCommittedResource(target)\n");
        exit(1);
    }
    if (rtv.ptr)
        ID3D12Device_CreateRenderTargetView(dev, res, NULL, rtv);
    return res;
}

static void copy_to_readback(ID3D12Resource *tex, ID3D12Resource *readback)
{
    D3D12_TEXTURE_COPY_LOCATION src = { tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    D3D12_TEXTURE_COPY_LOCATION dst = { readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = W;
    dst.PlacedFootprint.Footprint.Height = H;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = 256;
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
}

static void expect_pixel(const char *name, const unsigned char *p, unsigned r, unsigned g, unsigned b)
{
    if (abs(p[0] - (int)r) > 2 || abs(p[1] - (int)g) > 2 || abs(p[2] - (int)b) > 2)
    {
        printf("FAIL %s: %u %u %u, expected %u %u %u\n", name, p[0], p[1], p[2], r, g, b);
        failures++;
    }
}

int main(void)
{
    ID3D12DescriptorHeap *rtv_heap;
    ID3DBlob *blob = NULL, *err = NULL;
    HRESULT hr;
    unsigned char *mapped;

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

    /* --- buffer to buffer CopyTextureRegion: 8x4 RGBA8 source, box (2,1)-(6,3) to (3,2) of a
       16x8 destination at offset 512 */
    {
        ID3D12Resource *src = make_buffer(D3D12_HEAP_TYPE_UPLOAD, 1024);
        ID3D12Resource *dst = make_buffer(D3D12_HEAP_TYPE_READBACK, 4096);
        ID3D12Resource_Map(src, 0, NULL, (void **)&mapped);
        for (unsigned y = 0; y < 4; y++)
            for (unsigned x = 0; x < 8; x++)
                for (unsigned c = 0; c < 4; c++)
                    mapped[y * 256 + x * 4 + c] = (unsigned char)(y * 64 + x * 4 + c);
        ID3D12Resource_Unmap(src, 0, NULL);
        ID3D12Resource_Map(dst, 0, NULL, (void **)&mapped);
        memset(mapped, 0xee, 4096);
        D3D12_TEXTURE_COPY_LOCATION s = { src, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        s.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        s.PlacedFootprint.Footprint.Width = 8;
        s.PlacedFootprint.Footprint.Height = 4;
        s.PlacedFootprint.Footprint.Depth = 1;
        s.PlacedFootprint.Footprint.RowPitch = 256;
        D3D12_TEXTURE_COPY_LOCATION d = { dst, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        d.PlacedFootprint.Offset = 512;
        d.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.PlacedFootprint.Footprint.Width = 16;
        d.PlacedFootprint.Footprint.Height = 8;
        d.PlacedFootprint.Footprint.Depth = 1;
        d.PlacedFootprint.Footprint.RowPitch = 256;
        D3D12_BOX box = { 2, 1, 0, 6, 3, 1 };
        ID3D12GraphicsCommandList_CopyTextureRegion(list, &d, 3, 2, 0, &s, &box);
        submit_and_wait();
        int bad = 0;
        for (unsigned y = 0; y < 8; y++)
            for (unsigned x = 0; x < 16; x++)
            {
                const unsigned char *p = mapped + 512 + y * 256 + x * 4;
                int inside = x >= 3 && x < 7 && y >= 2 && y < 4;
                unsigned sx = x - 3 + 2, sy = y - 2 + 1;
                for (unsigned c = 0; c < 4; c++)
                {
                    unsigned want = inside ? (unsigned char)(sy * 64 + sx * 4 + c) : 0xee;
                    if (p[c] != want)
                        bad++;
                }
            }
        if (bad)
        {
            printf("FAIL buffer to buffer CopyTextureRegion: %d bytes differ\n", bad);
            failures++;
        }
        ID3D12Resource_Unmap(dst, 0, NULL);
    }

    /* --- WriteBufferImmediate into a default buffer, copied to readback */
    {
        ID3D12GraphicsCommandList2 *list2;
        ID3D12Resource *buf, *rb;
        D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC bd = { 0 };
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 256;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                             &IID_ID3D12Resource, (void **)&buf);
        rb = make_buffer(D3D12_HEAP_TYPE_READBACK, 256);
        if (FAILED(ID3D12GraphicsCommandList_QueryInterface(list, &IID_ID3D12GraphicsCommandList2, (void **)&list2)))
        {
            printf("FAIL no ID3D12GraphicsCommandList2\n");
            return 1;
        }
        D3D12_GPU_VIRTUAL_ADDRESS va = ID3D12Resource_GetGPUVirtualAddress(buf);
        D3D12_WRITEBUFFERIMMEDIATE_PARAMETER params[2] = { { va + 4, 0x12345678 }, { va + 64, 0xcafef00d } };
        D3D12_WRITEBUFFERIMMEDIATE_MODE modes[2] = { D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT,
                                                     D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT };
        ID3D12GraphicsCommandList2_WriteBufferImmediate(list2, 2, params, modes);
        ID3D12GraphicsCommandList_CopyBufferRegion(list, rb, 0, buf, 0, 256);
        submit_and_wait();
        UINT *v;
        ID3D12Resource_Map(rb, 0, NULL, (void **)&v);
        if (v[1] != 0x12345678 || v[16] != 0xcafef00d)
        {
            printf("FAIL WriteBufferImmediate: %08x %08x\n", v[1], v[16]);
            failures++;
        }
        ID3D12Resource_Unmap(rb, 0, NULL);
        ID3D12GraphicsCommandList2_Release(list2);
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3 };
    ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap);
    UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv0 = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv1 = { rtv0.ptr + inc }, rtv2 = { rtv0.ptr + 2 * inc }, no_rtv = { 0 };
    ID3D12Resource *readback = make_buffer(D3D12_HEAP_TYPE_READBACK, 256 * H);

    /* --- ResolveSubresourceRegion: 4x MSAA cleared to a color, resolved as a whole */
    {
        ID3D12GraphicsCommandList1 *list1;
        ID3D12Resource *ms = make_target(4, rtv1), *single = make_target(1, no_rtv);
        const float color[4] = { 0.2f, 0.6f, 0.8f, 1 };
        ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv1, color, 0, NULL);
        ID3D12GraphicsCommandList_QueryInterface(list, &IID_ID3D12GraphicsCommandList1, (void **)&list1);
        ID3D12GraphicsCommandList1_ResolveSubresourceRegion(list1, ms, 0, 0, 0, ms, 0, NULL,
                                                            DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOLVE_MODE_DECOMPRESS);
        ID3D12GraphicsCommandList1_ResolveSubresourceRegion(list1, single, 0, 0, 0, ms, 0, NULL,
                                                            DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOLVE_MODE_AVERAGE);
        copy_to_readback(single, readback);
        submit_and_wait();
        ID3D12Resource_Map(readback, 0, NULL, (void **)&mapped);
        expect_pixel("ResolveSubresourceRegion", mapped + 8 * 256 + 8 * 4, 51, 153, 204);
        ID3D12Resource_Unmap(readback, 0, NULL);
        ID3D12GraphicsCommandList1_Release(list1);
    }

    /* pipeline with a root constant color for the bundle and discard cases */
    ID3D12RootSignature *root;
    ID3D12PipelineState *pso;
    {
        D3D12_ROOT_PARAMETER param = { D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS };
        param.Constants.Num32BitValues = 4;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &param };
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
    }
    D3D12_VIEWPORT vp = { 0, 0, W, H, 0, 1 };
    D3D12_RECT sc = { 0, 0, W, H }, left = { 0, 0, W / 2, H };
    const float black[4] = { 0, 0, 0, 1 };

    /* --- ExecuteBundle: the bundle sets the pipeline (initial state), topology, root signature
       and a color, and draws; the calling list draws once more after it with the bundle's state */
    {
        ID3D12CommandAllocator *balloc;
        ID3D12GraphicsCommandList *bundle;
        ID3D12Resource *rt = make_target(1, rtv0);
        const float green[4] = { 0, 1, 0, 1 }, red[4] = { 1, 0, 0, 1 };
        if (FAILED(hr = ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_BUNDLE,
                &IID_ID3D12CommandAllocator, (void **)&balloc)) ||
            FAILED(hr = ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_BUNDLE, balloc, pso,
                &IID_ID3D12GraphicsCommandList, (void **)&bundle)))
        {
            printf("FAIL bundle creation: hr=0x%08lx\n", (unsigned long)hr);
            return 1;
        }
        if (ID3D12GraphicsCommandList_GetType(bundle) != D3D12_COMMAND_LIST_TYPE_BUNDLE)
        {
            printf("FAIL bundle GetType\n");
            failures++;
        }
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(bundle, root);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(bundle, 0, 4, green, 0);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(bundle, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_DrawInstanced(bundle, 3, 1, 0, 0);
        ID3D12GraphicsCommandList_Close(bundle);

        ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv0, black, 0, NULL);
        ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv0, FALSE, NULL);
        ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
        ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
        ID3D12GraphicsCommandList_ExecuteBundle(list, bundle);
        ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &left);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 4, red, 0);
        ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
        copy_to_readback(rt, readback);
        submit_and_wait();
        ID3D12Resource_Map(readback, 0, NULL, (void **)&mapped);
        expect_pixel("ExecuteBundle", mapped + 8 * 256 + 12 * 4, 0, 255, 0);
        expect_pixel("state after ExecuteBundle", mapped + 8 * 256 + 2 * 4, 255, 0, 0);
        ID3D12Resource_Unmap(readback, 0, NULL);
    }

    /* --- DiscardResource: draw blue, discard, clear black and draw white on the left half; the
       discard must not keep the next pass from working, and must not touch another target */
    {
        ID3D12Resource *rt = make_target(1, rtv2), *other = make_target(1, rtv1);
        const float blue[4] = { 0, 0, 1, 1 }, white[4] = { 1, 1, 1, 1 }, yellow[4] = { 1, 1, 0, 1 };
        ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv1, yellow, 0, NULL);
        ID3D12GraphicsCommandList_SetPipelineState(list, pso);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, root);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv2, FALSE, NULL);
        ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
        ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 4, blue, 0);
        ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
        ID3D12GraphicsCommandList_DiscardResource(list, rt, NULL);
        ID3D12GraphicsCommandList_DiscardResource(list, rt, NULL);
        ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv2, black, 0, NULL);
        ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &left);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 4, white, 0);
        ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
        copy_to_readback(rt, readback);
        submit_and_wait();
        ID3D12Resource_Map(readback, 0, NULL, (void **)&mapped);
        expect_pixel("clear and draw after DiscardResource", mapped + 8 * 256 + 2 * 4, 255, 255, 255);
        expect_pixel("clear and draw after DiscardResource", mapped + 8 * 256 + 12 * 4, 0, 0, 0);
        ID3D12Resource_Unmap(readback, 0, NULL);

        /* discard followed directly by a full-screen draw (load DontCare) */
        ID3D12GraphicsCommandList_SetPipelineState(list, pso);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, root);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv2, FALSE, NULL);
        ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
        ID3D12GraphicsCommandList_DiscardResource(list, rt, NULL);
        ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 4, blue, 0);
        ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
        copy_to_readback(rt, readback);
        submit_and_wait();
        ID3D12Resource_Map(readback, 0, NULL, (void **)&mapped);
        expect_pixel("draw after DiscardResource", mapped + 8 * 256 + 2 * 4, 0, 0, 255);
        expect_pixel("draw after DiscardResource", mapped + 8 * 256 + 12 * 4, 0, 0, 255);
        ID3D12Resource_Unmap(readback, 0, NULL);
        copy_to_readback(other, readback);
        submit_and_wait();
        ID3D12Resource_Map(readback, 0, NULL, (void **)&mapped);
        expect_pixel("other target untouched", mapped + 8 * 256 + 8 * 4, 255, 255, 0);
        ID3D12Resource_Unmap(readback, 0, NULL);
    }

    if (failures)
    {
        printf("FAIL %d checks\n", failures);
        return 2;
    }
    printf("PASS D3D12 command list methods\n");
    return 0;
}
