/* Native Metal regression for D3D12 mesh pipelines without a pixel shader.
 * Build: xcrun clang++ -std=c++17 -framework Foundation -framework Metal \
 *        tests/metal_mesh_no_pixel_shader.mm -o /tmp/metal-mesh-no-ps
 * Run without arguments to test the no-output fragment workaround. --repro
 * leaves the fragment function nil and reproduces Metal's validation abort.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cmath>
#include <cstdio>
#include <cstring>

int main(int argc, char **argv) {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device || ![device supportsFamily:MTLGPUFamilyApple7]) {
      fprintf(stderr, "SKIP: requires a mesh-capable Apple GPU\n");
      return 77;
    }
    NSError *error = nil;
    NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
      "struct V { float4 p [[position]]; };\n"
      "[[mesh]] void mesh_main(mesh<V,void,3,1,topology::triangle> o, "
      "uint t [[thread_index_in_threadgroup]]) { if(t==0) { "
      "o.set_vertex(0,V{float4(-1,-1,0.5,1)}); "
      "o.set_vertex(1,V{float4(0,1,0.5,1)}); "
      "o.set_vertex(2,V{float4(1,-1,0.5,1)}); "
      "o.set_index(0,0); o.set_index(1,1); o.set_index(2,2); o.set_primitive_count(1); } }\n"
      "[[fragment]] void fs_d3d12_no_pixel_shader() {}\n";
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    if (!library) { NSLog(@"FAIL shader compilation: %@", error); return 1; }
    MTLMeshRenderPipelineDescriptor *pd = [MTLMeshRenderPipelineDescriptor new];
    pd.meshFunction = [library newFunctionWithName:@"mesh_main"];
    pd.maxTotalThreadsPerMeshThreadgroup = 32;
    pd.rasterSampleCount = 1;
    pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    if (!(argc > 1 && !strcmp(argv[1], "--repro")))
      pd.fragmentFunction = [library newFunctionWithName:@"fs_d3d12_no_pixel_shader"];
    id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithMeshDescriptor:pd
        options:MTLPipelineOptionNone reflection:nil error:&error];
    if (!pipeline) { NSLog(@"FAIL pipeline: %@", error); return 1; }

    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
        width:16 height:16 mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> color = [device newTextureWithDescriptor:td];
    td.pixelFormat = MTLPixelFormatDepth32Float;
    id<MTLTexture> depth = [device newTextureWithDescriptor:td];
    id<MTLBuffer> readback = [device newBufferWithLength:8192 options:MTLResourceStorageModeShared];
    MTLDepthStencilDescriptor *ds = [MTLDepthStencilDescriptor new];
    ds.depthCompareFunction = MTLCompareFunctionAlways;
    ds.depthWriteEnabled = YES;
    id<MTLDepthStencilState> depthState = [device newDepthStencilStateWithDescriptor:ds];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = color;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 1, 1);
    pass.depthAttachment.texture = depth;
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    pass.depthAttachment.clearDepth = 1;
    id<MTLRenderCommandEncoder> render = [cb renderCommandEncoderWithDescriptor:pass];
    [render setRenderPipelineState:pipeline];
    [render setDepthStencilState:depthState];
    [render setCullMode:MTLCullModeNone];
    [render drawMeshThreadgroups:MTLSizeMake(1,1,1) threadsPerObjectThreadgroup:MTLSizeMake(1,1,1)
        threadsPerMeshThreadgroup:MTLSizeMake(1,1,1)];
    [render endEncoding];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    for (unsigned i = 0; i < 2; ++i)
      [blit copyFromTexture:i ? depth : color sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
          sourceSize:MTLSizeMake(16,16,1) toBuffer:readback destinationOffset:i * 4096
          destinationBytesPerRow:256 destinationBytesPerImage:4096];
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted) { NSLog(@"FAIL execution: %@", cb.error); return 1; }
    auto bytes = static_cast<unsigned char *>(readback.contents);
    for (unsigned y = 0; y < 16; ++y)
      for (unsigned x = 0; x < 16; ++x) {
        auto p = bytes + y * 256 + x * 4;
        if (p[0] != 255 || p[1] != 0 || p[2] != 255 || p[3] != 255) {
          fprintf(stderr, "FAIL no-PS draw modified color at %u,%u\n", x, y); return 1;
        }
      }
    float z;
    memcpy(&z, bytes + 4096 + 8 * 256 + 8 * 4, sizeof(z));
    if (std::fabs(z - 0.5f) > 0.0001f) {
      fprintf(stderr, "FAIL no-PS draw did not write depth: %f\n", z); return 1;
    }
    puts("PASS no-pixel-shader mesh draw preserves color and writes depth");
    return 0;
  }
}
