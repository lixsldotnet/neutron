/* neutron smoke test: D3D12 renders on Metal (through DXMT) without a window.
 * Creates a device, clears a render target to a known color, copies it to a
 * readback buffer and checks the pixel. Exit code 0 means the color matched. */
#define COBJMACROS
#define INITGUID
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdio.h>

#define W 64
#define H 64

static int fail(const char *what, HRESULT hr)
{
    printf("FAIL %s: hr=0x%08lx\n", what, (unsigned long)hr);
    return 1;
}

int main(void)
{
    ID3D12Device *dev = NULL;
    ID3D12CommandQueue *queue = NULL;
    ID3D12CommandAllocator *alloc = NULL;
    ID3D12GraphicsCommandList *list = NULL;
    ID3D12DescriptorHeap *rtv_heap = NULL;
    ID3D12Resource *target = NULL, *readback = NULL;
    ID3D12Fence *fence = NULL;
    HRESULT hr;

    if (FAILED(hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev)))
        return fail("D3D12CreateDevice", hr);
    printf("device ok\n");

    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    if (FAILED(hr = ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue)))
        return fail("CreateCommandQueue", hr);
    if (FAILED(hr = ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void **)&alloc)))
        return fail("CreateCommandAllocator", hr);
    if (FAILED(hr = ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL,
            &IID_ID3D12GraphicsCommandList, (void **)&list)))
        return fail("CreateCommandList", hr);

    D3D12_HEAP_PROPERTIES default_heap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = { 0 };
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = W;
    td.Height = H;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv = { DXGI_FORMAT_R8G8B8A8_UNORM, { { 0.2f, 0.4f, 0.6f, 1.0f } } };
    if (FAILED(hr = ID3D12Device_CreateCommittedResource(dev, &default_heap, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, &IID_ID3D12Resource, (void **)&target)))
        return fail("CreateCommittedResource(target)", hr);

    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    if (FAILED(hr = ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap)))
        return fail("CreateDescriptorHeap", hr);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap);
    ID3D12Device_CreateRenderTargetView(dev, target, NULL, rtv);

    /* Readback buffer: rows aligned to 256 bytes. */
    UINT row_pitch = (W * 4 + 255) & ~255u;
    D3D12_HEAP_PROPERTIES readback_heap = { D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC bd = { 0 };
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = (UINT64)row_pitch * H;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(hr = ID3D12Device_CreateCommittedResource(dev, &readback_heap, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&readback)))
        return fail("CreateCommittedResource(readback)", hr);

    const float color[4] = { 0.2f, 0.4f, 0.6f, 1.0f };
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, color, 0, NULL);

    D3D12_RESOURCE_BARRIER barrier = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    barrier.Transition.pResource = target;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION src = { target, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst = { readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = W;
    dst.PlacedFootprint.Footprint.Height = H;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = row_pitch;
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);

    if (FAILED(hr = ID3D12GraphicsCommandList_Close(list))) return fail("Close", hr);
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);

    if (FAILED(hr = ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence)))
        return fail("CreateFence", hr);
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    ID3D12CommandQueue_Signal(queue, fence, 1);
    ID3D12Fence_SetEventOnCompletion(fence, 1, ev);
    if (WaitForSingleObject(ev, 5000) != WAIT_OBJECT_0) return fail("GPU wait timed out", E_FAIL);

    unsigned char *pixels;
    D3D12_RANGE range = { 0, (SIZE_T)row_pitch * H };
    if (FAILED(hr = ID3D12Resource_Map(readback, 0, &range, (void **)&pixels))) return fail("Map", hr);
    unsigned char *p = pixels + (H / 2) * row_pitch + (W / 2) * 4;
    printf("pixel at center: %u %u %u %u (expect about 51 102 153 255)\n", p[0], p[1], p[2], p[3]);
    int ok = abs(p[0] - 51) <= 2 && abs(p[1] - 102) <= 2 && abs(p[2] - 153) <= 2 && p[3] == 255;
    printf(ok ? "PASS D3D12 clear + copy on Metal\n" : "FAIL wrong color\n");
    return ok ? 0 : 2;
}
