#include "BScopeView.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "Theme.hpp"

namespace radar::ui {

BScopeView::~BScopeView() {
    // Context may already be gone; release_gl() handles the normal path.
}

void BScopeView::release_gl() {
    if (tex_ != 0) {
        glDeleteTextures(1, &tex_);
        tex_ = 0;
    }
}

void BScopeView::init_gl() {
    if (tex_ == 0) {
        glGenTextures(1, &tex_);
        glBindTexture(GL_TEXTURE_2D, tex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kAzBins, kRangeBins, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
        rgba_.assign(size_t(kAzBins) * kRangeBins * 4, 0);
    }
    if (!lut_built_) {
        for (int i = 0; i < 256; ++i) {
            unsigned char r, g, b;
            theme::bscope_gradient(i / 255.0f, r, g, b);
            lut_[i][0] = r; lut_[i][1] = g; lut_[i][2] = b; lut_[i][3] = 255;
        }
        lut_built_ = true;
    }
}

void BScopeView::splat(const app::BlipView& b) {
    const int az = ((int)std::lround(b.azimuth_deg) % 360 + 360) % 360;
    const int rb = std::clamp((int)(b.range_m / kRangeMaxM * kRangeBins),
                              0, kRangeBins - 1);
    const float amp = std::clamp((float)(b.snr_db / 30.0), 0.15f, 1.0f);
    // 3x3 gaussian splat
    for (int da = -1; da <= 1; ++da) {
        for (int dr = -1; dr <= 1; ++dr) {
            const int a = (az + da + 360) % 360;
            const int r = rb + dr;
            if (r < 0 || r >= kRangeBins) continue;
            const float w = (da == 0 && dr == 0) ? 1.0f : 0.4f;
            float& cell = heat_[size_t(r) * kAzBins + a];
            cell = std::min(1.0f, cell + amp * w);
        }
    }
}

void BScopeView::render(const char* title, ImVec2 pos, ImVec2 size,
                        const std::vector<app::TrackView>& tracks,
                        const app::ShipView& ship,
                        bool sector_mode, double sector_center, double sector_width,
                        float dt) {
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::Begin(title, nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoResize);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = ImGui::GetContentRegionAvail().y - 4.0f;

    dl->AddRectFilled(wp, ImVec2(wp.x + w, wp.y + h), theme::col_bg());
    dl->AddRect(wp, ImVec2(wp.x + w, wp.y + h), theme::col_border());

    if (tex_ != 0) {
        // decay heat, then convert through the LUT and upload
        const float decay = std::pow(0.5f, dt / 4.0f);   // 4 s half-life
        for (size_t i = 0; i < heat_.size(); ++i) {
            heat_[i] *= decay;
            const unsigned char v = (unsigned char)std::clamp(heat_[i] * 255.0f, 0.0f, 255.0f);
            const auto* c = lut_[v];
            // row 0 of the texture = far range (top of display)
            rgba_[i*4+0] = c[0]; rgba_[i*4+1] = c[1];
            rgba_[i*4+2] = c[2]; rgba_[i*4+3] = c[3];
        }
        // flip vertically: heat_ row 0 = near range -> bottom of texture
        static thread_local std::vector<unsigned char> flipped;
        flipped.resize(rgba_.size());
        const size_t row_bytes = size_t(kAzBins) * 4;
        for (int r = 0; r < kRangeBins; ++r)
            std::copy_n(rgba_.data() + size_t(r) * row_bytes, row_bytes,
                        flipped.data() + size_t(kRangeBins - 1 - r) * row_bytes);

        glBindTexture(GL_TEXTURE_2D, tex_);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kAzBins, kRangeBins,
                        GL_RGBA, GL_UNSIGNED_BYTE, flipped.data());
        glBindTexture(GL_TEXTURE_2D, 0);

        ImGui::Image((ImTextureID)(intptr_t)tex_, ImVec2(w, h));
    }

    // --- overlay mapping helpers (screen space) ---
    auto x_of_az = [&](double az) { return wp.x + (float)(az / 360.0) * w; };
    auto y_of_range = [&](double r) {
        return wp.y + h - (float)(r / kRangeMaxM) * h;
    };

    // dashed sector boundary lines
    if (sector_mode) {
        for (double edge : {sector_center - sector_width / 2, sector_center + sector_width / 2}) {
            double a = std::fmod(edge + 360.0, 360.0);
            const float x = x_of_az(a);
            for (float y = wp.y; y < wp.y + h; y += 8.0f)
                dl->AddLine(ImVec2(x, y), ImVec2(x, std::min(y + 4.0f, wp.y + h)),
                            IM_COL32(255, 220, 80, 160), 1.0f);
        }
    }

    // track markers with IDs
    for (const auto& t : tracks) {
        const double range = std::hypot(t.x_m, t.y_m);
        if (range > kRangeMaxM) continue;
        const double az_world = std::atan2(t.x_m, t.y_m) * 180.0 / 3.14159265358979323846;
        double az_ship = std::fmod(az_world - ship.heading_deg + 360.0, 360.0);
        const ImVec2 p(x_of_az(az_ship), y_of_range(range));
        dl->AddRect(ImVec2(p.x - 4, p.y - 4), ImVec2(p.x + 4, p.y + 4),
                    theme::col_track(), 0.0f, 0, 1.5f);
        char lbl[16];
        std::snprintf(lbl, sizeof lbl, "%lld", (long long)t.track_id);
        dl->AddText(ImVec2(p.x + 6, p.y - 6), theme::col_text(), lbl);
    }

    // axis labels
    char lbl[32];
    for (int deg = 0; deg < 360; deg += 60) {
        std::snprintf(lbl, sizeof lbl, "%03d", deg);
        dl->AddText(ImVec2(x_of_az(deg) - 8, wp.y + h - 14), theme::col_text_dim(), lbl);
    }
    std::snprintf(lbl, sizeof lbl, "%.0fkm", kRangeMaxM / 1000.0);
    dl->AddText(ImVec2(wp.x + 4, wp.y + 4), theme::col_text_dim(), lbl);

    ImGui::Dummy(ImVec2(w, h));
    ImGui::End();
}

} // namespace radar::ui
