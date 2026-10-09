/* neutron headless test: D3D12 tessellation (hull and domain shaders, DXBC from d3dcompiler)
 * through DXMT. DXMT runs VS+HS as the object function and the DS as the mesh function of a
 * Metal mesh pipeline. Draws into an offscreen target and reads it back. Cases:
 *   tri      triangle patch, constant factors, DS color from SV_DomainLocation, patch
 *            constant from a root CBV visible to the HS only
 *   quad     quad patch whose top edge the DS bends up (only visible when the patch is
 *            really tessellated), factor from root constants (HS), bend from root
 *            constants (DS), vertex buffer + input layout
 *   indexed  two quad patches from a 32-bit index buffer with base vertex, and a 16-bit one
 *   inst     two instances of a patch (SV_InstanceID in the VS)
 * Exit code 0 = all pixels match.
 * Build: x86_64-w64-mingw32-clang -O2 -o d3d12_tess.exe d3d12_tess.c -ld3d12 -ld3dcompiler
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
    "cbuffer HsCb : register(b0) { float4 tint; };\n"
    "cbuffer HsConst : register(b1) { float factor; float3 pad1; };\n"
    "cbuffer DsConst : register(b2) { float bend; float3 pad2; };\n"
    "struct VIn { float2 pos : POSITION; float4 col : COLOR; };\n"
    "struct CP { float4 pos : POS; float4 col : COLOR; };\n"
    "CP vs(VIn i, uint inst : SV_InstanceID) {\n"
    "  CP o; o.pos = float4(i.pos + float2(1.0 * inst, 0), 0, 1); o.col = i.col; return o; }\n"
    /* triangle domain */
    "struct TriPC { float e[3] : SV_TessFactor; float i : SV_InsideTessFactor; float4 tint : TINT; };\n"
    "TriPC tri_pc(InputPatch<CP, 3> p) { TriPC o; o.e[0] = o.e[1] = o.e[2] = 4; o.i = 4; o.tint = tint; return o; }\n"
    "[domain(\"tri\")] [partitioning(\"integer\")] [outputtopology(\"triangle_cw\")] [outputcontrolpoints(3)]\n"
    "[patchconstantfunc(\"tri_pc\")] CP tri_hs(InputPatch<CP, 3> p, uint i : SV_OutputControlPointID) { return p[i]; }\n"
    "struct DSOut { float4 pos : SV_Position; float4 col : COLOR; };\n"
    "[domain(\"tri\")] DSOut tri_ds(TriPC pc, float3 uvw : SV_DomainLocation, const OutputPatch<CP, 3> p) {\n"
    "  DSOut o; o.pos = p[0].pos * uvw.x + p[1].pos * uvw.y + p[2].pos * uvw.z;\n"
    "  o.col = float4(uvw, 1) * pc.tint; return o; }\n"
    /* quad domain */
    "struct QuadPC { float e[4] : SV_TessFactor; float i[2] : SV_InsideTessFactor; };\n"
    "QuadPC quad_pc(InputPatch<CP, 4> p) { QuadPC o; o.e[0] = o.e[1] = o.e[2] = o.e[3] = factor;\n"
    "  o.i[0] = o.i[1] = factor; return o; }\n"
    "[domain(\"quad\")] [partitioning(\"integer\")] [outputtopology(\"triangle_cw\")] [outputcontrolpoints(4)]\n"
    "[patchconstantfunc(\"quad_pc\")] CP quad_hs(InputPatch<CP, 4> p, uint i : SV_OutputControlPointID) { return p[i]; }\n"
    /* control points: 0 = top left, 1 = top right, 2 = bottom left, 3 = bottom right */
    "[domain(\"quad\")] DSOut quad_ds(QuadPC pc, float2 uv : SV_DomainLocation, const OutputPatch<CP, 4> p) {\n"
    "  DSOut o; float4 top = lerp(p[0].pos, p[1].pos, uv.x), bottom = lerp(p[2].pos, p[3].pos, uv.x);\n"
    "  o.pos = lerp(top, bottom, uv.y);\n"
    "  o.pos.y += bend * sin(3.14159265 * uv.x) * (1 - uv.y);\n"
    "  o.col = p[0].col; return o; }\n"
    "float4 ps(DSOut i) : SV_Target { return i.col; }\n";

static ID3DBlob *compile(const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *errors = NULL;
    HRESULT hr = D3DCompile(shader_src, sizeof(shader_src) - 1, "tess", NULL, NULL, entry, target, 0, 0, &code,
                            &errors);
    if (FAILED(hr))
    {
        printf("compile %s failed: %s\n", entry, errors ? (char *)ID3D10Blob_GetBufferPointer(errors) : "?");
        exit(1);
    }
    return code;
}

static D3D12_SHADER_BYTECODE bc(ID3DBlob *b)
{
    D3D12_SHADER_BYTECODE r = { ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b) };
    return r;
}

static ID3D12Resource *make_buffer(D3D12_HEAP_TYPE type, UINT64 size, const void *data)
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
            type == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_GENERIC_READ,
            NULL, &IID_ID3D12Resource, (void **)&res)))
    {
        printf("FAIL CreateCommittedResource(buffer)\n");
        exit(1);
    }
    if (data)
    {
        void *p;
        ID3D12Resource_Map(res, 0, NULL, &p);
        memcpy(p, data, size);
        ID3D12Resource_Unmap(res, 0, NULL);
    }
    return res;
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

static ID3D12Resource *target, *readback;
static D3D12_CPU_DESCRIPTOR_HANDLE rtv;

static void begin_target(void)
{
    const float black[4] = { 0, 0, 0, 1 };
    D3D12_VIEWPORT vp = { 0, 0, W, H, 0, 1 };
    D3D12_RECT sc = { 0, 0, W, H };
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, black, 0, NULL);
    ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
}

static unsigned char *read_target(void)
{
    void *p;
    D3D12_TEXTURE_COPY_LOCATION src = { target, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    D3D12_TEXTURE_COPY_LOCATION dst = { readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    src.SubresourceIndex = 0;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = W;
    dst.PlacedFootprint.Footprint.Height = H;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = W * 4;
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
    submit_and_wait();
    ID3D12Resource_Map(readback, 0, NULL, &p);
    return p;
}

/* pixel at NDC (x, y) */
static void expect(const char *name, unsigned char *pixels, float x, float y, unsigned r, unsigned g, unsigned b,
                   int tolerance)
{
    int px = (int)((x * 0.5f + 0.5f) * W), py = (int)((0.5f - y * 0.5f) * H);
    if (px >= W) px = W - 1;
    if (py >= H) py = H - 1;
    unsigned char *p = pixels + (py * W + px) * 4;
    int ok = abs(p[0] - (int)r) <= tolerance && abs(p[1] - (int)g) <= tolerance && abs(p[2] - (int)b) <= tolerance;
    if (!ok)
    {
        printf("FAIL %s: pixel (%.2f,%.2f) = %u %u %u, expected %u %u %u\n", name, x, y, p[0], p[1], p[2], r, g, b);
        failures++;
    }
}

static ID3D12PipelineState *make_pso(ID3D12RootSignature *root, ID3DBlob *vs, ID3DBlob *hs, ID3DBlob *ds,
                                     ID3DBlob *ps)
{
    static const D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = { 0 };
    ID3D12PipelineState *pso;
    HRESULT hr;
    pd.pRootSignature = root;
    pd.VS = bc(vs);
    pd.HS = bc(hs);
    pd.DS = bc(ds);
    pd.PS = bc(ps);
    pd.InputLayout.pInputElementDescs = layout;
    pd.InputLayout.NumElements = 2;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = 0xffffffff;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    if (FAILED(hr = ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso)))
    {
        printf("FAIL CreateGraphicsPipelineState: hr=0x%08lx\n", (unsigned long)hr);
        exit(1);
    }
    return pso;
}

struct vertex
{
    float x, y;
    float r, g, b, a;
};

int main(void)
{
    ID3D12RootSignature *root;
    ID3DBlob *blob = NULL, *err = NULL;
    ID3D12DescriptorHeap *rtv_heap;
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

    /* root signature: 0 = CBV b0 (HS), 1 = constants b1 (HS), 2 = constants b2 (DS) */
    D3D12_ROOT_PARAMETER params[3] = { 0 };
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_HULL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 1;
    params[1].Constants.Num32BitValues = 4;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_HULL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.ShaderRegister = 2;
    params[2].Constants.Num32BitValues = 4;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_DOMAIN;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 3, params, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
    if (FAILED(hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
        FAILED(hr = ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob),
                                                     ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature,
                                                     (void **)&root)))
    {
        printf("FAIL root signature: hr=0x%08lx\n", (unsigned long)hr);
        return 1;
    }

    ID3DBlob *vs = compile("vs", "vs_5_0"), *ps = compile("ps", "ps_5_0");
    ID3DBlob *tri_hs = compile("tri_hs", "hs_5_0"), *tri_ds = compile("tri_ds", "ds_5_0");
    ID3DBlob *quad_hs = compile("quad_hs", "hs_5_0"), *quad_ds = compile("quad_ds", "ds_5_0");
    printf("shaders compiled (DXBC)\n");

    ID3D12PipelineState *pso_tri = make_pso(root, vs, tri_hs, tri_ds, ps);
    ID3D12PipelineState *pso_quad = make_pso(root, vs, quad_hs, quad_ds, ps);
    printf("pipelines ok\n");

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
    rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap);
    ID3D12Device_CreateRenderTargetView(dev, target, NULL, rtv);
    readback = make_buffer(D3D12_HEAP_TYPE_READBACK, W * 4 * H, NULL);

    static const struct vertex verts[] = {
        /* 0: full-screen-ish triangle for the tri patch */
        { -1, -1, 1, 1, 1, 1 }, { 0, 1, 1, 1, 1, 1 }, { 1, -1, 1, 1, 1, 1 },
        /* 3: quad patch x -0.5..0.5, y -0.5..0.5 (TL, TR, BL, BR), red */
        { -0.5f, 0.5f, 1, 0, 0, 1 }, { 0.5f, 0.5f, 1, 0, 0, 1 }, { -0.5f, -0.5f, 1, 0, 0, 1 },
        { 0.5f, -0.5f, 1, 0, 0, 1 },
        /* 7: small quad patches, left half: x -0.9..-0.6, green / blue */
        { -0.9f, 0.9f, 0, 1, 0, 1 }, { -0.6f, 0.9f, 0, 1, 0, 1 }, { -0.9f, 0.6f, 0, 1, 0, 1 }, { -0.6f, 0.6f, 0, 1, 0, 1 },
        { -0.9f, -0.6f, 0, 0, 1, 1 }, { -0.6f, -0.6f, 0, 0, 1, 1 }, { -0.9f, -0.9f, 0, 0, 1, 1 },
        { -0.6f, -0.9f, 0, 0, 1, 1 },
    };
    ID3D12Resource *vb = make_buffer(D3D12_HEAP_TYPE_UPLOAD, sizeof(verts), verts);
    D3D12_VERTEX_BUFFER_VIEW vbv = { ID3D12Resource_GetGPUVirtualAddress(vb), sizeof(verts), sizeof(struct vertex) };

    const float cb_data[64] = { 1, 1, 1, 1 };
    ID3D12Resource *cb = make_buffer(D3D12_HEAP_TYPE_UPLOAD, 256, cb_data);
    const float half_tint[64] = { 0.5f, 1, 1, 1 };
    ID3D12Resource *cb_half = make_buffer(D3D12_HEAP_TYPE_UPLOAD, 256, half_tint);

    /* 32-bit: 8 indices relative to base vertex 7 (both small patches); 16-bit: the second one */
    static const UINT idx32[] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    static const UINT16 idx16[] = { 11, 12, 13, 14 };
    ID3D12Resource *ib32 = make_buffer(D3D12_HEAP_TYPE_UPLOAD, sizeof(idx32), idx32);
    ID3D12Resource *ib16 = make_buffer(D3D12_HEAP_TYPE_UPLOAD, 256, idx16);
    D3D12_INDEX_BUFFER_VIEW ibv32 = { ID3D12Resource_GetGPUVirtualAddress(ib32), sizeof(idx32), DXGI_FORMAT_R32_UINT };
    D3D12_INDEX_BUFFER_VIEW ibv16 = { ID3D12Resource_GetGPUVirtualAddress(ib16), sizeof(idx16), DXGI_FORMAT_R16_UINT };

    const float factor16[4] = { 16, 0, 0, 0 }, factor1[4] = { 1, 0, 0, 0 };
    const float bend[4] = { 0.4f, 0, 0, 0 }, no_bend[4] = { 0, 0, 0, 0 };
    unsigned char *px;

#define SETUP(pso, cbuf)                                                                                             \
    do                                                                                                               \
    {                                                                                                                \
        ID3D12GraphicsCommandList_SetPipelineState(list, pso);                                                       \
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, root);                                              \
        ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(list, 0,                                         \
                                                                    ID3D12Resource_GetGPUVirtualAddress(cbuf));      \
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 1, 4, factor16, 0);                            \
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 2, 4, bend, 0);                                \
        ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);                                              \
    } while (0)

    /* tri: color = barycentrics * tint; the centroid is (1/3, 1/3, 1/3) */
    begin_target();
    SETUP(pso_tri, cb);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
    px = read_target();
    expect("tri", px, 0, -1.0f / 3, 85, 85, 85, 6);
    expect("tri", px, -0.7f, -0.9f, 210, 13, 32, 12); /* uvw about (0.825, 0.05, 0.125) */
    expect("tri", px, -0.9f, 0.9f, 0, 0, 0, 2);
    ID3D12Resource_Unmap(readback, 0, NULL);

    /* tri with the patch constant tint (0.5, 1, 1) from another CBV */
    begin_target();
    SETUP(pso_tri, cb_half);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
    px = read_target();
    expect("tri_tint", px, 0, -1.0f / 3, 43, 85, 85, 6);
    ID3D12Resource_Unmap(readback, 0, NULL);

    /* quad, factor 16: the bent top edge reaches y = 0.9 at x = 0 */
    begin_target();
    SETUP(pso_quad, cb);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST);
    ID3D12GraphicsCommandList_DrawInstanced(list, 4, 1, 3, 0);
    px = read_target();
    expect("quad", px, 0, 0, 255, 0, 0, 2);
    expect("quad", px, 0, 0.75f, 255, 0, 0, 2);
    expect("quad", px, 0, 0.97f, 0, 0, 0, 2);
    expect("quad", px, 0.75f, 0, 0, 0, 0, 2);
    ID3D12Resource_Unmap(readback, 0, NULL);

    /* quad, factor 1: no inner vertices, the top edge stays straight */
    begin_target();
    SETUP(pso_quad, cb);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 1, 4, factor1, 0);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST);
    ID3D12GraphicsCommandList_DrawInstanced(list, 4, 1, 3, 0);
    px = read_target();
    expect("quad_f1", px, 0, 0, 255, 0, 0, 2);
    expect("quad_f1", px, 0, 0.75f, 0, 0, 0, 2);
    ID3D12Resource_Unmap(readback, 0, NULL);

    /* indexed: 32-bit indices 0..7 + base vertex 7 (two patches), then 16-bit {11..14}
       with two instances and start instance 1 (SV_InstanceID still counts from 0: the
       copies are at x + 0 and x + 1) */
    begin_target();
    SETUP(pso_quad, cb);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 2, 4, no_bend, 0);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST);
    ID3D12GraphicsCommandList_IASetIndexBuffer(list, &ibv32);
    ID3D12GraphicsCommandList_DrawIndexedInstanced(list, 8, 1, 0, 7, 0);
    ID3D12GraphicsCommandList_IASetIndexBuffer(list, &ibv16);
    ID3D12GraphicsCommandList_DrawIndexedInstanced(list, 4, 2, 0, 0, 1); /* instance 1 and 2: x + 1, x + 2 */
    px = read_target();
    expect("indexed32", px, -0.75f, 0.75f, 0, 255, 0, 2);
    expect("indexed32", px, -0.75f, -0.75f, 0, 0, 255, 2);
    expect("indexed16", px, 0.25f, -0.75f, 0, 0, 255, 2);
    expect("indexed16", px, 0.95f, -0.75f, 0, 0, 0, 2);
    expect("indexed", px, 0.25f, 0.75f, 0, 0, 0, 2);
    ID3D12Resource_Unmap(readback, 0, NULL);

    /* inst: the quad patch twice, the second instance moved right by 1 (half off screen) */
    begin_target();
    SETUP(pso_quad, cb);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 2, 4, no_bend, 0);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST);
    ID3D12GraphicsCommandList_DrawInstanced(list, 4, 2, 3, 0);
    px = read_target();
    expect("inst", px, -0.25f, 0, 255, 0, 0, 2);
    expect("inst", px, 0.75f, 0, 255, 0, 0, 2);
    expect("inst", px, -0.75f, 0, 0, 0, 0, 2);
    ID3D12Resource_Unmap(readback, 0, NULL);

    if (failures)
    {
        printf("FAIL %d pixel checks\n", failures);
        return 2;
    }
    printf("PASS D3D12 tessellation\n");
    return 0;
}
