/* neutron headless test: D3D12 geometry shaders (DXBC from d3dcompiler) through DXMT.
 * DXMT runs a GS as a Metal mesh pipeline (VS in the object stage, GS in the mesh stage).
 * Draws into an offscreen target and reads it back. Cases:
 *   points   point list -> quads (triangle strip output), vertex buffer + input layout,
 *            root CBV and root constants visible to the GS only, root constants for the PS
 *   indexed  same pipeline, 16-bit and 32-bit index buffers, base vertex
 *   strip    triangle strip input, SV_PrimitiveID in the GS
 *   inst     draw instancing (SV_InstanceID in the VS), two strips per GS invocation
 *   lines    line list input expanded to thick quads, descriptor table SRV read in the GS
 *   layer    GS that sets SV_RenderTargetArrayIndex, as a loop (mesh pipeline) and as plain
 *            moves (pass-through, folded into the VS like DXMT's D3D11 does)
 * Exit code 0 = all pixels match.
 * Build: x86_64-w64-mingw32-clang -O2 -o d3d12_gs.exe d3d12_gs.c -ld3d12 -ld3dcompiler
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
    "cbuffer GsCb : register(b0) { float size; float3 pad; };\n"
    "cbuffer GsConst : register(b1) { float4 gs_color; };\n"
    "cbuffer PsConst : register(b2) { float4 ps_mul; };\n"
    "Buffer<float4> offsets : register(t0);\n"
    "struct VIn { float2 pos : POSITION; float4 col : COLOR; };\n"
    "struct V { float4 pos : SV_Position; float4 col : COLOR; };\n"
    "struct G { float4 pos : SV_Position; float4 col : COLOR; };\n"
    "V vs(VIn i) { V o; o.pos = float4(i.pos, 0, 1); o.col = i.col; return o; }\n"
    "V vs_inst(VIn i, uint inst : SV_InstanceID) {\n"
    "  V o; o.pos = float4(i.pos + float2(0, -1.0 * inst), 0, 1); o.col = i.col; return o; }\n"
    /* points -> quads */
    "[maxvertexcount(4)] void gs_quad(point V i[1], inout TriangleStream<G> s) {\n"
    "  G o; o.col = i[0].col * gs_color;\n"
    "  o.pos = i[0].pos + float4(-size, -size, 0, 0); s.Append(o);\n"
    "  o.pos = i[0].pos + float4(-size,  size, 0, 0); s.Append(o);\n"
    "  o.pos = i[0].pos + float4( size, -size, 0, 0); s.Append(o);\n"
    "  o.pos = i[0].pos + float4( size,  size, 0, 0); s.Append(o);\n"
    "}\n"
    /* triangles, colored by primitive id */
    "[maxvertexcount(3)] void gs_prim(triangle V i[3], uint id : SV_PrimitiveID, inout TriangleStream<G> s) {\n"
    "  for (int k = 0; k < 3; k++) { G o; o.pos = i[k].pos;\n"
    "    o.col = id == 0 ? float4(1, 0, 0, 1) : (id == 1 ? float4(0, 1, 0, 1) : float4(0, 0, 1, 1));\n"
    "    s.Append(o); }\n"
    "  s.RestartStrip();\n"
    "}\n"
    /* two strips per point: the second one moved right by 1 and yellow (RestartStrip) */
    "[maxvertexcount(8)] void gs_inst(point V i[1], inout TriangleStream<G> s) {\n"
    "  for (uint gi = 0; gi < 2; gi++) {\n"
    "    G o; o.col = gi == 0 ? i[0].col : float4(1, 1, 0, 1);\n"
    "    float4 c = i[0].pos + float4(1.0 * gi, 0, 0, 0);\n"
    "    o.pos = c + float4(-0.25, -0.25, 0, 0); s.Append(o);\n"
    "    o.pos = c + float4(-0.25,  0.25, 0, 0); s.Append(o);\n"
    "    o.pos = c + float4( 0.25, -0.25, 0, 0); s.Append(o);\n"
    "    o.pos = c + float4( 0.25,  0.25, 0, 0); s.Append(o);\n"
    "    s.RestartStrip();\n"
    "  }\n"
    "}\n"
    /* lines -> thick horizontal bands, offset read from an SRV */
    "[maxvertexcount(4)] void gs_line(line V i[2], uint id : SV_PrimitiveID, inout TriangleStream<G> s) {\n"
    "  float4 d = offsets[id];\n"
    "  G o; o.col = i[0].col;\n"
    "  o.pos = i[0].pos + d + float4(0, -0.1, 0, 0); s.Append(o);\n"
    "  o.pos = i[0].pos + d + float4(0,  0.1, 0, 0); s.Append(o);\n"
    "  o.pos = i[1].pos + d + float4(0, -0.1, 0, 0); s.Append(o);\n"
    "  o.pos = i[1].pos + d + float4(0,  0.1, 0, 0); s.Append(o);\n"
    "}\n"
    "float4 ps(G i) : SV_Target { return i.col * ps_mul; }\n"
    /* layer: VS picks the layer, GS only passes it through */
    "struct LV { float4 pos : SV_Position; uint layer : LAYER; };\n"
    "struct LG { float4 pos : SV_Position; uint layer : SV_RenderTargetArrayIndex; };\n"
    "LV vs_layer(uint id : SV_VertexID) { LV o; float2 uv = float2((id << 1) & 2, id & 2);\n"
    "  o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.layer = 1; return o; }\n"
    "[maxvertexcount(3)] void gs_layer(triangle LV i[3], inout TriangleStream<LG> s) {\n"
    "  for (int k = 0; k < 3; k++) { LG o; o.pos = i[k].pos; o.layer = i[k].layer; s.Append(o); } }\n"
    /* only moves and emits: DXMT folds it into the vertex shader */
    "[maxvertexcount(3)] void gs_layer_pt(triangle LV i[3], inout TriangleStream<LG> s) {\n"
    "  LG o; o.pos = i[0].pos; o.layer = i[0].layer; s.Append(o);\n"
    "  o.pos = i[1].pos; o.layer = i[1].layer; s.Append(o);\n"
    "  o.pos = i[2].pos; o.layer = i[2].layer; s.Append(o); }\n"
    "float4 ps_layer(LG i) : SV_Target { return float4(0, 0.5, 1, 1); }\n";

static ID3DBlob *compile(const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *errors = NULL;
    HRESULT hr = D3DCompile(shader_src, sizeof(shader_src) - 1, "gs", NULL, NULL, entry, target, 0, 0, &code, &errors);
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

/* render target (optionally an array) with an RTV, cleared, and a readback of one slice */
struct target
{
    ID3D12Resource *tex, *readback;
    ID3D12DescriptorHeap *heap;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    UINT slices;
};

static void make_target(struct target *t, UINT slices)
{
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = { 0 };
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    D3D12_RENDER_TARGET_VIEW_DESC rd = { 0 };
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = W;
    td.Height = H;
    td.DepthOrArraySize = slices;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    t->slices = slices;
    if (FAILED(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&t->tex)))
    {
        printf("FAIL CreateCommittedResource(target)\n");
        exit(1);
    }
    ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&t->heap);
    t->rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(t->heap);
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (slices > 1)
    {
        rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rd.Texture2DArray.ArraySize = slices;
    }
    else
        rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    ID3D12Device_CreateRenderTargetView(dev, t->tex, &rd, t->rtv);
    t->readback = make_buffer(D3D12_HEAP_TYPE_READBACK, (UINT64)W * 4 * H * slices, NULL);
}

static void begin_target(struct target *t)
{
    const float black[4] = { 0, 0, 0, 1 };
    D3D12_VIEWPORT vp = { 0, 0, W, H, 0, 1 };
    D3D12_RECT sc = { 0, 0, W, H };
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, t->rtv, black, 0, NULL);
    ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &t->rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
}

static unsigned char *read_target(struct target *t)
{
    void *p;
    for (UINT s = 0; s < t->slices; s++)
    {
        D3D12_TEXTURE_COPY_LOCATION src = { t->tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        D3D12_TEXTURE_COPY_LOCATION dst = { t->readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        src.SubresourceIndex = s;
        dst.PlacedFootprint.Offset = (UINT64)W * 4 * H * s;
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        dst.PlacedFootprint.Footprint.Width = W;
        dst.PlacedFootprint.Footprint.Height = H;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = W * 4;
        ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
    }
    submit_and_wait();
    ID3D12Resource_Map(t->readback, 0, NULL, &p);
    return p;
}

/* pixel at NDC (x, y) of slice s */
static void expect(const char *name, unsigned char *pixels, UINT s, float x, float y, unsigned r, unsigned g,
                   unsigned b)
{
    int px = (int)((x * 0.5f + 0.5f) * W), py = (int)((0.5f - y * 0.5f) * H);
    if (px >= W) px = W - 1;
    if (py >= H) py = H - 1;
    unsigned char *p = pixels + (UINT64)W * 4 * H * s + (py * W + px) * 4;
    int ok = abs(p[0] - (int)r) <= 2 && abs(p[1] - (int)g) <= 2 && abs(p[2] - (int)b) <= 2;
    if (!ok)
    {
        printf("FAIL %s: pixel (%.2f,%.2f) slice %u = %u %u %u, expected %u %u %u\n", name, x, y, s, p[0], p[1],
               p[2], r, g, b);
        failures++;
    }
}

static ID3D12PipelineState *make_pso(ID3D12RootSignature *root, ID3DBlob *vs, ID3DBlob *gs, ID3DBlob *ps,
                                     D3D12_PRIMITIVE_TOPOLOGY_TYPE topo, BOOL input_layout)
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
    if (gs) pd.GS = bc(gs);
    pd.PS = bc(ps);
    if (input_layout)
    {
        pd.InputLayout.pInputElementDescs = layout;
        pd.InputLayout.NumElements = 2;
    }
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = 0xffffffff;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = topo;
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
    ID3D12DescriptorHeap *srv_heap;
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

    /* root signature: 0 = CBV b0 (GS), 1 = constants b1 (GS), 2 = constants b2 (PS), 3 = table t0 (GS) */
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
    D3D12_ROOT_PARAMETER params[4] = { 0 };
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_GEOMETRY;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 1;
    params[1].Constants.Num32BitValues = 4;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_GEOMETRY;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.ShaderRegister = 2;
    params[2].Constants.Num32BitValues = 4;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 1;
    params[3].DescriptorTable.pDescriptorRanges = &range;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_GEOMETRY;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 4, params, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
    if (FAILED(hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
        FAILED(hr = ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob),
                                                     ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature,
                                                     (void **)&root)))
    {
        printf("FAIL root signature: hr=0x%08lx\n", (unsigned long)hr);
        return 1;
    }

    ID3DBlob *vs = compile("vs", "vs_5_0"), *vs_inst = compile("vs_inst", "vs_5_0");
    ID3DBlob *gs_quad = compile("gs_quad", "gs_5_0"), *gs_prim = compile("gs_prim", "gs_5_0");
    ID3DBlob *gs_inst = compile("gs_inst", "gs_5_0"), *gs_line = compile("gs_line", "gs_5_0");
    ID3DBlob *ps = compile("ps", "ps_5_0");
    ID3DBlob *vs_layer = compile("vs_layer", "vs_5_0"), *gs_layer = compile("gs_layer", "gs_5_0");
    ID3DBlob *ps_layer = compile("ps_layer", "ps_5_0"), *gs_layer_pt = compile("gs_layer_pt", "gs_5_0");
    printf("shaders compiled (DXBC)\n");

    ID3D12PipelineState *pso_quad = make_pso(root, vs, gs_quad, ps, D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT, TRUE);
    ID3D12PipelineState *pso_prim = make_pso(root, vs, gs_prim, ps, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, TRUE);
    ID3D12PipelineState *pso_inst = make_pso(root, vs_inst, gs_inst, ps, D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT, TRUE);
    ID3D12PipelineState *pso_line = make_pso(root, vs, gs_line, ps, D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE, TRUE);
    ID3D12PipelineState *pso_plain = make_pso(root, vs, NULL, ps, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, TRUE);
    ID3D12PipelineState *pso_layer = make_pso(root, vs_layer, gs_layer, ps_layer,
                                              D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, FALSE);
    ID3D12PipelineState *pso_layer_pt = make_pso(root, vs_layer, gs_layer_pt, ps_layer,
                                                 D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, FALSE);
    printf("pipelines ok\n");

    /* vertices: 4 quadrant centers (points), then a strip quad, then two lines */
    static const struct vertex verts[] = {
        { -0.5f, 0.5f, 1, 0, 0, 1 }, { 0.5f, 0.5f, 0, 1, 0, 1 }, { -0.5f, -0.5f, 0, 0, 1, 1 },
        { 0.5f, -0.5f, 1, 1, 1, 1 },
        /* 4: strip quad covering the screen: tri 0 = upper left, tri 1 = lower right */
        { -1, 1, 0.2f, 0.4f, 0.6f, 1 }, { 1, 1, 0.2f, 0.4f, 0.6f, 1 }, { -1, -1, 0.2f, 0.4f, 0.6f, 1 },
        { 1, -1, 0.2f, 0.4f, 0.6f, 1 },
        /* 8: two lines from x=-0.8 to 0.8 at y=0 (moved by the SRV offsets) */
        { -0.8f, 0, 1, 0, 1, 1 }, { 0.8f, 0, 1, 0, 1, 1 }, { -0.8f, 0, 0, 1, 1, 1 }, { 0.8f, 0, 0, 1, 1, 1 },
    };
    ID3D12Resource *vb = make_buffer(D3D12_HEAP_TYPE_UPLOAD, sizeof(verts), verts);
    D3D12_VERTEX_BUFFER_VIEW vbv = { ID3D12Resource_GetGPUVirtualAddress(vb), sizeof(verts), sizeof(struct vertex) };

    const float cb_data[64] = { 0.25f };
    ID3D12Resource *cb = make_buffer(D3D12_HEAP_TYPE_UPLOAD, 256, cb_data);

    static const UINT16 idx16[] = { 3, 0 };
    static const UINT idx32[] = { 0, 1, 2 }; /* with base vertex 1: vertices 1, 2, 3 */
    ID3D12Resource *ib16 = make_buffer(D3D12_HEAP_TYPE_UPLOAD, sizeof(idx16), idx16);
    ID3D12Resource *ib32 = make_buffer(D3D12_HEAP_TYPE_UPLOAD, 256, idx32);
    D3D12_INDEX_BUFFER_VIEW ibv16 = { ID3D12Resource_GetGPUVirtualAddress(ib16), sizeof(idx16), DXGI_FORMAT_R16_UINT };
    D3D12_INDEX_BUFFER_VIEW ibv32 = { ID3D12Resource_GetGPUVirtualAddress(ib32), sizeof(idx32), DXGI_FORMAT_R32_UINT };

    /* SRV: float4 offsets per line primitive */
    static const float offsets[8] = { 0, 0.5f, 0, 0, 0, -0.5f, 0, 0 };
    ID3D12Resource *offs = make_buffer(D3D12_HEAP_TYPE_UPLOAD, sizeof(offsets), offsets);
    D3D12_DESCRIPTOR_HEAP_DESC shd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12Device_CreateDescriptorHeap(dev, &shd, &IID_ID3D12DescriptorHeap, (void **)&srv_heap);
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = { 0 };
    sd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Buffer.NumElements = 2;
    ID3D12Device_CreateShaderResourceView(dev, offs, &sd,
                                          ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(srv_heap));

    const float gs_color[4] = { 1, 1, 1, 1 }, gs_half[4] = { 0.5f, 0.5f, 0.5f, 1 }, ps_mul[4] = { 1, 1, 1, 1 };
    struct target t, tl;
    unsigned char *px;
    make_target(&t, 1);
    make_target(&tl, 2);

#define SETUP(pso)                                                                                                   \
    do                                                                                                               \
    {                                                                                                                \
        ID3D12GraphicsCommandList_SetPipelineState(list, pso);                                                       \
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, root);                                              \
        ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &srv_heap);                                            \
        ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(list, 0, ID3D12Resource_GetGPUVirtualAddress(cb)); \
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 1, 4, gs_color, 0);                            \
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 2, 4, ps_mul, 0);                              \
        ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(                                                    \
            list, 3, ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(srv_heap));                             \
        ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);                                              \
    } while (0)

    /* points: 4 quads of half size 0.25 around the quadrant centers */
    begin_target(&t);
    SETUP(pso_quad);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    ID3D12GraphicsCommandList_DrawInstanced(list, 4, 1, 0, 0);
    px = read_target(&t);
    expect("points", px, 0, -0.5f, 0.5f, 255, 0, 0);
    expect("points", px, 0, 0.5f, 0.5f, 0, 255, 0);
    expect("points", px, 0, -0.5f, -0.5f, 0, 0, 255);
    expect("points", px, 0, 0.5f, -0.5f, 255, 255, 255);
    expect("points", px, 0, -0.5f, 0.85f, 0, 0, 0);
    expect("points", px, 0, 0, 0, 0, 0, 0);
    ID3D12Resource_Unmap(t.readback, 0, NULL);

    /* indexed: 16-bit {3, 0}, then 32-bit with base vertex 1 (vertices 1..3) and half color
       from the GS root constants; the 32-bit draw comes second and overwrites quad 3 */
    begin_target(&t);
    SETUP(pso_quad);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    ID3D12GraphicsCommandList_IASetIndexBuffer(list, &ibv16);
    ID3D12GraphicsCommandList_DrawIndexedInstanced(list, 2, 1, 0, 0, 0);
    ID3D12GraphicsCommandList_IASetIndexBuffer(list, &ibv32);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 1, 4, gs_half, 0);
    ID3D12GraphicsCommandList_DrawIndexedInstanced(list, 2, 1, 1, 1, 0); /* indices 1, 2 + 1 = vertices 2, 3 */
    px = read_target(&t);
    expect("indexed16", px, 0, -0.5f, 0.5f, 255, 0, 0);
    expect("indexed16", px, 0, 0.5f, 0.5f, 0, 0, 0);
    expect("indexed32", px, 0, -0.5f, -0.5f, 0, 0, 128);
    expect("indexed32", px, 0, 0.5f, -0.5f, 128, 128, 128);
    ID3D12Resource_Unmap(t.readback, 0, NULL);

    /* strip: 4 vertices as a triangle strip, the GS colors by primitive id */
    begin_target(&t);
    SETUP(pso_prim);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D12GraphicsCommandList_DrawInstanced(list, 4, 1, 4, 0);
    px = read_target(&t);
    expect("strip", px, 0, -0.6f, 0.6f, 255, 0, 0);
    expect("strip", px, 0, 0.6f, -0.6f, 0, 255, 0);
    ID3D12Resource_Unmap(t.readback, 0, NULL);

    /* same pipeline as a triangle list: one triangle (id 0) and nothing else */
    begin_target(&t);
    SETUP(pso_prim);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 5, 0); /* (1,1) (-1,-1) (1,-1): lower right */
    px = read_target(&t);
    expect("list", px, 0, 0.6f, -0.6f, 255, 0, 0);
    expect("list", px, 0, -0.6f, 0.6f, 0, 0, 0);
    ID3D12Resource_Unmap(t.readback, 0, NULL);

    /* inst: point 0 (-0.5, 0.5), 2 draw instances (second one moved down by 1), 2 strips
       per point (second one moved right by 1, yellow) */
    begin_target(&t);
    SETUP(pso_inst);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    ID3D12GraphicsCommandList_DrawInstanced(list, 1, 2, 0, 0);
    px = read_target(&t);
    expect("inst", px, 0, -0.5f, 0.5f, 255, 0, 0);
    expect("inst", px, 0, 0.5f, 0.5f, 255, 255, 0);
    expect("inst", px, 0, -0.5f, -0.5f, 255, 0, 0);
    expect("inst", px, 0, 0.5f, -0.5f, 255, 255, 0);
    ID3D12Resource_Unmap(t.readback, 0, NULL);

    /* lines: two lines, moved up and down by 0.5 by the SRV */
    begin_target(&t);
    SETUP(pso_line);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_LINELIST);
    ID3D12GraphicsCommandList_DrawInstanced(list, 4, 1, 8, 0);
    px = read_target(&t);
    expect("lines", px, 0, 0, 0.5f, 255, 0, 255);
    expect("lines", px, 0, 0, -0.5f, 0, 255, 255);
    expect("lines", px, 0, 0, 0, 0, 0, 0);
    ID3D12Resource_Unmap(t.readback, 0, NULL);

    /* mixed: GS draw, ordinary draw, GS draw in the same pass (lower right half in the
       strip color, then quad 3 on top) */
    begin_target(&t);
    SETUP(pso_quad);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    ID3D12GraphicsCommandList_DrawInstanced(list, 1, 1, 0, 0);
    ID3D12GraphicsCommandList_SetPipelineState(list, pso_plain);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 5, 0);
    ID3D12GraphicsCommandList_SetPipelineState(list, pso_quad);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    ID3D12GraphicsCommandList_DrawInstanced(list, 1, 1, 3, 0);
    px = read_target(&t);
    expect("mixed", px, 0, -0.5f, 0.5f, 255, 0, 0);
    expect("mixed", px, 0, 0.9f, 0.5f, 51, 102, 153);
    expect("mixed", px, 0, 0.5f, -0.5f, 255, 255, 255);
    expect("mixed", px, 0, -0.5f, 0, 0, 0, 0);
    ID3D12Resource_Unmap(t.readback, 0, NULL);

    /* layer: full-screen triangle into slice 1 of a 2-slice array */
    begin_target(&tl);
    SETUP(pso_layer);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
    px = read_target(&tl);
    expect("layer", px, 1, 0, 0, 0, 128, 255);
    expect("layer", px, 0, 0, 0, 0, 0, 0);
    ID3D12Resource_Unmap(tl.readback, 0, NULL);

    begin_target(&tl);
    SETUP(pso_layer_pt);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
    px = read_target(&tl);
    expect("layer_pt", px, 1, 0, 0, 0, 128, 255);
    expect("layer_pt", px, 0, 0, 0, 0, 0, 0);
    ID3D12Resource_Unmap(tl.readback, 0, NULL);

    if (failures)
    {
        printf("FAIL %d pixel checks\n", failures);
        return 2;
    }
    printf("PASS D3D12 geometry shaders\n");
    return 0;
}
