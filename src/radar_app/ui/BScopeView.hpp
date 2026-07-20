#pragma once
// B-scope: range (Y) vs. azimuth (X) 2D sector intensity display.
//  - 360 x 256 intensity buffer, decaying phosphor-style
//  - detections splatted as small gaussian spots (amplitude from SNR)
//  - black -> blue -> green -> yellow -> red gradient via LUT
//  - uploaded to a GPU texture once per frame, drawn as an ImGui::Image
//  - overlaid: track markers with IDs, dashed sector boundary lines

#include <vector>

#include <imgui.h>
#if !defined(__APPLE__)
#include <GLFW/glfw3.h>   // GL texture handle
#endif

#include "../DataBus.hpp"

namespace radar::ui {

class MetalContext; // Apple: fwd-decl keeps this header GL/Metal-free

class BScopeView {
public:
    BScopeView() = default;
    ~BScopeView();

#if defined(__APPLE__)
    void init_texture(MetalContext& ctx);    // call AFTER Metal is up
    void release_texture(MetalContext& ctx);
#else
    void init_gl();   // call AFTER the GL context is current
    void release_gl(); // call BEFORE the GL context is destroyed
#endif
    void splat(const app::BlipView& b);
    // Crash-investigation knob: upload the heat texture every n-th frame
    // instead of every frame (values < 1 are treated as 1).
    void set_upload_decimation(int n) { upload_decimation_ = n < 1 ? 1 : n; }
    void render(const char* title, ImVec2 pos, ImVec2 size,
                const std::vector<app::TrackView>& tracks,
                const app::ShipView& ship,
                bool sector_mode, double sector_center, double sector_width,
                float dt);

private:
    static constexpr int kAzBins    = 360;   // 1 deg per column, ship-relative
    static constexpr int kRangeBins = 256;
    static constexpr double kRangeMaxM = 100000.0;

    // Parens, NOT braces: heat_{count, 0.0f} would select the
    // initializer_list<float> ctor (count converts to float without
    // narrowing) -> a TWO-element vector {92160, 0}. Every splat() then
    // wrote up to ~368 KB past the tiny buffer into random heap (or read
    // it) — the windowed-crash corruptor found by ASan on 2026-07-20.
    std::vector<float> heat_ = std::vector<float>(size_t(kAzBins) * kRangeBins, 0.0f);
    std::vector<unsigned char> rgba_;       // lazily sized W*H*4
    unsigned char lut_[256][4]{};           // gradient lookup table
#if defined(__APPLE__)
    void* tex_ = nullptr;                   // bridged MTLTexture
    MetalContext* ctx_ = nullptr;
#else
    GLuint tex_ = 0;
#endif
    bool lut_built_ = false;
    // Upload decimation: 1 = upload every frame (default); 4 = 15 Hz.
    int upload_decimation_ = 1;
    int upload_frame_ = 0;
};

} // namespace radar::ui
