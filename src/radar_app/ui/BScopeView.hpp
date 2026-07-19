#pragma once
// B-scope: range (Y) vs. azimuth (X) 2D sector intensity display.
//  - 360 x 256 intensity buffer, decaying phosphor-style
//  - detections splatted as small gaussian spots (amplitude from SNR)
//  - black -> blue -> green -> yellow -> red gradient via LUT
//  - uploaded to a GL texture once per frame, drawn as an ImGui::Image
//  - overlaid: track markers with IDs, dashed sector boundary lines

#include <vector>

#include <imgui.h>
#include <GLFW/glfw3.h>   // GL texture handle

#include "../DataBus.hpp"

namespace radar::ui {

class BScopeView {
public:
    BScopeView() = default;
    ~BScopeView();

    void init_gl();   // call AFTER the GL context is current
    void release_gl(); // call BEFORE the GL context is destroyed
    void splat(const app::BlipView& b);
    void render(const char* title, ImVec2 pos, ImVec2 size,
                const std::vector<app::TrackView>& tracks,
                const app::ShipView& ship,
                bool sector_mode, double sector_center, double sector_width,
                float dt);

private:
    static constexpr int kAzBins    = 360;   // 1 deg per column, ship-relative
    static constexpr int kRangeBins = 256;
    static constexpr double kRangeMaxM = 100000.0;

    std::vector<float> heat_{size_t(kAzBins) * kRangeBins, 0.0f};
    std::vector<unsigned char> rgba_;       // lazily sized W*H*4
    unsigned char lut_[256][4]{};           // gradient lookup table
    GLuint tex_ = 0;
    bool lut_built_ = false;
};

} // namespace radar::ui
