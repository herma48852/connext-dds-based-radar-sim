// MetalContext implementation (Apple only, Objective-C++, ARC).
// See MetalContext.hpp for the why: no OpenGL context, no GL->Metal shim.

#include "MetalContext.hpp"

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <imgui.h>
#include <backends/imgui_impl_metal.h>

namespace radar::ui {

struct MetalContext::Impl {
    GLFWwindow*             window = nullptr;
    id<MTLDevice>           device;
    id<MTLCommandQueue>     queue;
    CAMetalLayer*           layer = nil;
    MTLRenderPassDescriptor* pass = nil;
    id<CAMetalDrawable>     drawable;   // acquired per frame
};

MetalContext::~MetalContext() { shutdown(); }

bool MetalContext::init(GLFWwindow* window) {
    impl_ = new Impl();
    impl_->window = window;

    impl_->device = MTLCreateSystemDefaultDevice();
    if (!impl_->device) return false;
    impl_->queue = [impl_->device newCommandQueue];
    if (!impl_->queue) return false;

    // GLFW was created with GLFW_CLIENT_API = GLFW_NO_API, so the content
    // view is a plain NSView — safe to make it layer-backed with Metal.
    NSWindow* nswin = glfwGetCocoaWindow(window);
    impl_->layer = [CAMetalLayer layer];
    impl_->layer.device = impl_->device;
    impl_->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    impl_->layer.contentsScale = nswin.backingScaleFactor;
    [nswin.contentView setLayer:impl_->layer];
    [nswin.contentView setWantsLayer:YES];

    impl_->pass = [MTLRenderPassDescriptor renderPassDescriptor];
    impl_->pass.colorAttachments[0].loadAction  = MTLLoadActionClear;
    impl_->pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    // Same clear color as the old glClearColor.
    impl_->pass.colorAttachments[0].clearColor =
        MTLClearColorMake(0.04, 0.04, 0.06, 1.0);

    return ImGui_ImplMetal_Init(impl_->device);
}

void MetalContext::shutdown() {
    if (!impl_) return;
    ImGui_ImplMetal_Shutdown();
    delete impl_;   // ARC releases the ObjC members
    impl_ = nullptr;
}

bool MetalContext::begin_frame() {
    int fb_w = 0, fb_h = 0;
    glfwGetFramebufferSize(impl_->window, &fb_w, &fb_h);
    if (fb_w <= 0 || fb_h <= 0) return false;   // minimized / hidden

    NSWindow* nswin = glfwGetCocoaWindow(impl_->window);
    impl_->layer.contentsScale = nswin.backingScaleFactor;
    impl_->layer.drawableSize = CGSizeMake((CGFloat)fb_w, (CGFloat)fb_h);

    // nextDrawable blocks when 3 frames are in flight -> natural vsync,
    // replacing glfwSwapInterval(1).
    impl_->drawable = [impl_->layer nextDrawable];
    return impl_->drawable != nil;
}

void MetalContext::new_frame() {
    impl_->pass.colorAttachments[0].texture = impl_->drawable.texture;
    ImGui_ImplMetal_NewFrame(impl_->pass);
}

void MetalContext::render() {
    id<MTLCommandBuffer> cb = [impl_->queue commandBuffer];
    id<MTLRenderCommandEncoder> enc =
        [cb renderCommandEncoderWithDescriptor:impl_->pass];
    ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cb, enc);
    [enc endEncoding];
    [cb presentDrawable:impl_->drawable];
    [cb commit];
    impl_->drawable = nil;
}

void* MetalContext::create_texture(int width, int height) {
    MTLTextureDescriptor* d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:(NSUInteger)width
                                                          height:(NSUInteger)height
                                                       mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> tex = [impl_->device newTextureWithDescriptor:d];
    // Managed/shared storage: replaceRegion is synchronized with GPU reads
    // at command-buffer boundaries, so per-frame re-upload is race-free.
    return (void*)CFBridgingRetain(tex);
}

void MetalContext::upload_texture(void* tex, const unsigned char* rgba,
                                  int width, int height) {
    if (!tex) return;
    id<MTLTexture> t = (__bridge id<MTLTexture>)tex;
    [t replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)width, (NSUInteger)height)
         mipmapLevel:0
           withBytes:rgba
         bytesPerRow:(NSUInteger)width * 4];
}

void MetalContext::destroy_texture(void* tex) {
    if (tex) CFBridgingRelease(tex);
}

} // namespace radar::ui
