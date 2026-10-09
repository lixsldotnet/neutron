/* neutron headless test: create D3D12 graphics pipelines from DXIL shader files.
 * No window, no draw. Usage: d3d12_dxil_pso.exe <vs.dxil> <ps.dxil>...
 * The root signature comes from the first pixel shader blob ([RootSignature] / -rootsig-define).
 * Pipelines use 4 RGBA16F targets and a D32 depth buffer, no input layout.
 * Prints one line per pixel shader; exit code = number of failures.
 * Build: x86_64-w64-mingw32-clang -O2 -o d3d12_dxil_pso.exe d3d12_dxil_pso.c -ld3d12 */
#define COBJMACROS
#define INITGUID
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <d3d12.h>
#include <stdio.h>
#include <stdlib.h>

static void *load(const char *path, SIZE_T *size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *data = malloc(*size);
    fread(data, 1, *size, f);
    fclose(f);
    return data;
}

int main(int argc, char **argv)
{
    ID3D12Device *dev = NULL;
    HRESULT hr;
    int failures = 0;
    if (argc < 3) {
        printf("usage: %s vs.dxil ps.dxil...\n", argv[0]);
        return 1;
    }
    if (FAILED(hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev))) {
        printf("FAIL D3D12CreateDevice hr=0x%08lx\n", (unsigned long)hr);
        return 1;
    }
    SIZE_T vs_size;
    void *vs = load(argv[1], &vs_size);
    if (!vs) {
        printf("FAIL cannot read %s\n", argv[1]);
        return 1;
    }
    for (int i = 2; i < argc; i++) {
        SIZE_T ps_size;
        void *ps = load(argv[i], &ps_size);
        ID3D12RootSignature *rs = NULL;
        ID3D12PipelineState *pso = NULL;
        if (!ps) {
            printf("%s: cannot read\n", argv[i]);
            failures++;
            continue;
        }
        if (FAILED(hr = ID3D12Device_CreateRootSignature(dev, 0, ps, ps_size, &IID_ID3D12RootSignature, (void **)&rs))) {
            printf("%s: CreateRootSignature hr=0x%08lx\n", argv[i], (unsigned long)hr);
            failures++;
            continue;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = { 0 };
        pd.pRootSignature = rs;
        pd.VS.pShaderBytecode = vs;
        pd.VS.BytecodeLength = vs_size;
        pd.PS.pShaderBytecode = ps;
        pd.PS.BytecodeLength = ps_size;
        for (int t = 0; t < 4; t++) {
            pd.BlendState.RenderTarget[t].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            pd.RTVFormats[t] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        }
        pd.SampleMask = ~0u;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthEnable = TRUE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 4;
        pd.SampleDesc.Count = 1;
        hr = ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso);
        printf("%s: %s hr=0x%08lx\n", argv[i], SUCCEEDED(hr) ? "ok" : "FAIL", (unsigned long)hr);
        if (FAILED(hr))
            failures++;
        if (pso)
            ID3D12PipelineState_Release(pso);
        ID3D12RootSignature_Release(rs);
        free(ps);
    }
    return failures;
}
