#pragma once
// MetalContext (Apple only): owns the Metal device, command queue and the
// CAMetalLayer attached to the GLFW window, and drives the ImGui Metal
// backend. Pure C++ interface — the Objective-C++ implementation lives in
// MetalContext.mm.
//
// This replaces the deprecated OpenGL->Metal translation layer
// (AppleMetalOpenGLRenderer/AGXMetalG16X), the prime suspect in the
// windowed-mode heap corruption: random victims, ASan-invisible, immune to
// CPU watchpoints. GLFW runs with GLFW_CLIENT_API = GLFW_NO_API here; no
// OpenGL context exists anywhere in the process.

struct GLFWwindow;

namespace radar::ui {

class MetalContext {
public:
    MetalContext() = default;
    ~MetalContext();                                 // releases GPU objects
    MetalContext(const MetalContext&) = delete;
    MetalContext& operator=(const MetalContext&) = delete;

    bool init(GLFWwindow* window);  // attach CAMetalLayer, ImGui_ImplMetal_Init
    void shutdown();                // ImGui_ImplMetal_Shutdown + GPU objects

    // Frame lifecycle. begin_frame() acquires the next drawable and sizes
    // the layer to the framebuffer (handles resize / Retina monitor moves);
    // returns false when minimized — the caller should pump events and skip
    // the ImGui frame entirely.
    bool begin_frame();
    void new_frame();               // ImGui_ImplMetal_NewFrame(pass descriptor)
    void render();                  // encode draw data, present drawable, commit

    // Re-upload ImGui's font atlas after a Retina-density change. A call
    // before init() is a harmless no-op; the first NewFrame builds it.
    bool rebuild_font_texture();

    // RGBA8 texture helpers (B-scope heat map). create_texture returns an
    // ImTextureID-compatible handle (bridged MTLTexture, owned by the
    // caller until destroy_texture).
    void* create_texture(int width, int height);
    void  upload_texture(void* tex, const unsigned char* rgba,
                         int width, int height);
    void  destroy_texture(void* tex);

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace radar::ui
