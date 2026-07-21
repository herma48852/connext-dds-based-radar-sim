#include "AScopeView.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "Theme.hpp"

namespace radar::ui {

void AScopeView::render(const char* title, ImVec2 pos, ImVec2 size,
                        const app::TraceBuffer& trace, float dt) {
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);

    char heading[96];
    if (!trace.magnitude.empty()) {
        az_deg_ = trace.azimuth_deg;
        el_deg_ = trace.elevation_deg;
        range_max_m_ = trace.range_max_m;
    }
    // Keep the visible beam readout dynamic while giving ImGui a stable
    // window identity. Without the ### suffix, every az/el change creates a
    // newly appearing window; focusing it clears ActiveID held by buttons in
    // the SCENARIOS and ARRAY FACE windows before mouse release.
    std::snprintf(heading, sizeof heading,
                  "%s  AZ %05.1f  EL %04.1f###A_SCOPE", title,
                  az_deg_, el_deg_);

    ImGui::Begin(heading, nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoResize);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = ImGui::GetContentRegionAvail().y - 4.0f;
    const float bottom = wp.y + h;

    dl->AddRectFilled(wp, ImVec2(wp.x + w, bottom), theme::col_bg());
    dl->AddRect(wp, ImVec2(wp.x + w, bottom), theme::col_border());

    // --- grid: 4 horizontal, 10 vertical with range labels ---
    for (int i = 1; i < 4; ++i) {
        const float y = bottom - h * i / 4.0f;
        dl->AddLine(ImVec2(wp.x, y), ImVec2(wp.x + w, y), theme::col_grid(), 1.0f);
    }
    for (int i = 0; i <= 10; ++i) {
        const float x = wp.x + w * i / 10.0f;
        dl->AddLine(ImVec2(x, wp.y), ImVec2(x, bottom), theme::col_grid(), 1.0f);
        char lbl[16];
        std::snprintf(lbl, sizeof lbl, "%.0f", range_max_m_ * i / 10.0 / 1000.0);
        dl->AddText(ImVec2(x + 2, bottom - 14), theme::col_text_dim(), lbl);
    }

    const auto& mag = trace.magnitude;
    const int n = (int)mag.size();
    if (n < 3) { ImGui::End(); return; }

    // --- phosphor persistence: decay old, max with new ---
    if (phosphor_.size() != mag.size()) phosphor_.assign(n, 0.0f);
    const float decay = std::exp(-2.5f * dt);   // ~0.4 s time constant
    for (int i = 0; i < n; ++i)
        phosphor_[i] = std::max(phosphor_[i] * decay, mag[i]);

    // --- trace polyline (glow pass, then core pass) ---
    auto y_of = [&](float v) {
        const float t = std::min(1.0f, v / kDisplayFullScale);
        return bottom - t * (h - 6.0f);
    };
    // precompute screen points once (no per-frame heap churn)
    static thread_local std::vector<ImVec2> pts;
    pts.resize(n);
    for (int i = 0; i < n; ++i)
        pts[i] = ImVec2(wp.x + (float)i / (n - 1) * w, y_of(phosphor_[i]));

    dl->AddPolyline(pts.data(), n, IM_COL32(60, 200, 110, 70), 0, 4.0f);   // glow
    dl->AddPolyline(pts.data(), n, theme::col_phosphor(), 0, 1.5f);        // core

    // --- peak-hold markers + strongest-return range gate ---
    float strongest = 0.0f;
    int strongest_i = -1;
    for (int i = 1; i < n - 1; ++i) {
        if (phosphor_[i] > kDetectThreshold &&
            phosphor_[i] >= phosphor_[i-1] && phosphor_[i] > phosphor_[i+1]) {
            const float x = pts[i].x, y = pts[i].y;
            // small triangle marker above the peak
            dl->AddTriangleFilled(ImVec2(x, y - 4), ImVec2(x - 4, y - 12),
                                  ImVec2(x + 4, y - 12),
                                  IM_COL32(255, 240, 120, 220));
            if (phosphor_[i] > strongest) { strongest = phosphor_[i]; strongest_i = i; }
        }
    }
    if (strongest_i >= 0) {
        // range-gate bracket: +/- 3 bins around the strongest peak
        const float x1 = pts[std::max(0, strongest_i - 3)].x;
        const float x2 = pts[std::min(n - 1, strongest_i + 3)].x;
        const ImU32 gate = IM_COL32(120, 220, 255, 180);
        dl->AddLine(ImVec2(x1, wp.y + 4), ImVec2(x1, bottom - 4), gate, 1.0f);
        dl->AddLine(ImVec2(x2, wp.y + 4), ImVec2(x2, bottom - 4), gate, 1.0f);
        dl->AddLine(ImVec2(x1, wp.y + 4), ImVec2(x1 + 6, wp.y + 4), gate, 1.0f);
        dl->AddLine(ImVec2(x2, wp.y + 4), ImVec2(x2 - 6, wp.y + 4), gate, 1.0f);
        char lbl[32];
        std::snprintf(lbl, sizeof lbl, "GATE %.1f km",
                      (double)strongest_i / n * range_max_m_ / 1000.0);
        dl->AddText(ImVec2(x2 + 6, wp.y + 6), IM_COL32(120, 220, 255, 255), lbl);
    }

    ImGui::Dummy(ImVec2(w, h));
    ImGui::End();
}

} // namespace radar::ui
