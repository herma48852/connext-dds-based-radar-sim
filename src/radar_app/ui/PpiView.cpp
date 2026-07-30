#include "PpiView.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "Theme.hpp"

namespace radar::ui {

namespace {
constexpr double kDeg2Rad = 3.14159265358979323846 / 180.0;
constexpr int64_t kBlipPersistMs = 6000;

// ship-relative ENU (east,north) -> PPI screen point (north-up)
inline ImVec2 enu_to_screen(double cx, double cy, double radius_px,
                            double x_east, double y_north, double range_m) {
    const double k = radius_px / range_m;
    return ImVec2((float)(cx + x_east * k), (float)(cy - y_north * k));
}

inline ImU32 with_alpha(ImU32 c, int a) {
    // IM_COL32_A_MASK is the ALPHA mask (0xFF000000): keep RGB via its
    // complement. (The old code masked WITH it, discarding RGB -> black.)
    a = a < 0 ? 0 : (a > 255 ? 255 : a);
    return (c & ~IM_COL32_A_MASK) | ((ImU32)a << IM_COL32_A_SHIFT);
}
} // namespace

void PpiView::add_blip(const app::BlipView& b) {
    if (blips_.size() >= 2048) blips_.pop_front();   // pooled, no allocation
    blips_.push_back(b);
}

void PpiView::update_track_trail(int64_t id, double x, double y) {
    auto& pts = trails_[id].pts;
    if (pts.empty() || pts.back().first != x || pts.back().second != y) {
        pts.push_back({x, y});
        if (pts.size() > 10) pts.pop_front();
    }
}

void PpiView::prune_tracks(const std::vector<app::TrackView>& live) {
    for (auto it = trails_.begin(); it != trails_.end();) {
        const bool found = std::any_of(live.begin(), live.end(),
            [&](const app::TrackView& t) { return t.track_id == it->first; });
        it = found ? std::next(it) : trails_.erase(it);
    }
}

void PpiView::render(const char* title, ImVec2 pos, ImVec2 size,
                     const std::vector<app::TrackView>& tracks,
                     const app::ShipView& ship,
                     const std::array<double, faces::kFaceCount>& sweep_az_deg,
                     const std::array<uint32_t, faces::kFaceCount>& rma_masks,
                     const std::array<int32_t, faces::kFaceCount>& radar_modes,
                     const std::array<double, faces::kFaceCount>&
                         sector_centers_deg,
                     const std::array<double, faces::kFaceCount>&
                         sector_widths_deg,
                     int32_t selected_face_id,
                     int64_t now_ms, float dt,
                     PanelFocusState* focus) {
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::Begin(title, nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoResize);
    render_panel_focus_control(PanelId::Ppi, focus);
    const float visual_scale =
        focus && focus->is_focused(PanelId::Ppi)
        ? PanelFocusState::presenter_scale() : 1.0f;

    // --- smooth zoom (mouse wheel over the scope) ---
    if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0.0f) {
        range_m_target_ *= (ImGui::GetIO().MouseWheel > 0 ? 0.9 : 1.1);
        range_m_target_ = std::clamp(range_m_target_, 10000.0, 100000.0);
    }
    range_m_smooth_ += (range_m_target_ - range_m_smooth_) * std::min(1.0f, dt * 8.0f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Geometry from the CONTENT area (not the whole window incl. title
    // bar), with the top line reserved for the HDG/SPD/RNG readout so it
    // can never collide with the scope or the azimuth labels.
    const ImVec2 wp = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = ImGui::GetContentRegionAvail().y;
    const float readout_h = ImGui::GetTextLineHeightWithSpacing();
    const double cx = wp.x + w * 0.5;
    const double cy = wp.y + readout_h + (h - readout_h) * 0.5;
    const double R  = std::min((double)w, (double)(h - readout_h)) * 0.44;

    if (R < 20.0) { ImGui::End(); return; } // too small to draw meaningfully

    dl->AddRectFilled(wp, ImVec2(wp.x + w, wp.y + h), theme::col_bg());

    // --- range rings + labels ---
    for (int i = 1; i <= 4; ++i) {
        const float r = (float)(R * i / 4.0);
        dl->AddCircle(
            ImVec2((float)cx, (float)cy), r, theme::col_ring(), 128,
            1.0f * visual_scale);
        char lbl[32];
        std::snprintf(lbl, sizeof lbl, "%.0f km", range_m_smooth_ * i / 4.0 / 1000.0);
        dl->AddText(ImVec2((float)cx + 4, (float)(cy - r) - 14),
                    theme::col_text_dim(), lbl);
    }

    // --- four physical face sectors (ship-relative, rotating with heading) ---
    for (const auto& face : faces::kDefinitions) {
        const auto face_index = static_cast<std::size_t>(face.id);
        const bool selected = face.id == selected_face_id;
        const bool offline =
            (rma_masks[face_index] & 0xFFFFu) == 0xFFFFu;
        const bool sector_mode = radar_modes[face_index] == 1;
        const ImU32 face_color = offline
            ? theme::col_led_fault() : theme::col_sweep();
        const ImU32 mode_color = sector_mode
            ? theme::col_led_warn() : face_color;

        if (selected || offline) {
            dl->PathClear();
            dl->PathLineTo(ImVec2((float)cx, (float)cy));
            constexpr int kSegments = 18;
            for (int segment = 0; segment <= kSegments; ++segment) {
                const double fraction =
                    static_cast<double>(segment) / kSegments;
                const double azimuth =
                    face.coverage_start_deg
                    + fraction
                        * (face.coverage_end_deg
                           - face.coverage_start_deg)
                    + ship.heading_deg;
                const double angle = azimuth * kDeg2Rad;
                dl->PathLineTo(ImVec2(
                    (float)(cx + R * std::sin(angle)),
                    (float)(cy - R * std::cos(angle))));
            }
            dl->PathFillConvex(
                with_alpha(face_color, offline ? 28 : 16));
        }

        const double start_angle =
            (face.coverage_start_deg + ship.heading_deg) * kDeg2Rad;
        dl->AddLine(
            ImVec2((float)cx, (float)cy),
            ImVec2((float)(cx + R * std::sin(start_angle)),
                   (float)(cy - R * std::cos(start_angle))),
            with_alpha(face_color, selected ? 150 : 75),
            (selected ? 1.6f : 1.0f) * visual_scale);

        // Sector Scan is persistent per face, independent of which face is
        // selected in the controls. Show every active sector as a face-local
        // amber wedge with boundary rays so the PPI reflects that retained
        // scheduler state instead of implying that only the selected face is
        // in Sector Scan.
        if (sector_mode) {
            const double half_width = sector_widths_deg[face_index] * 0.5;
            const double sector_start =
                sector_centers_deg[face_index] - half_width;
            const double sector_end =
                sector_centers_deg[face_index] + half_width;

            dl->PathClear();
            dl->PathLineTo(ImVec2((float)cx, (float)cy));
            constexpr int kSectorSegments = 12;
            for (int segment = 0; segment <= kSectorSegments; ++segment) {
                const double fraction =
                    static_cast<double>(segment) / kSectorSegments;
                const double azimuth =
                    sector_start
                    + fraction * (sector_end - sector_start)
                    + ship.heading_deg;
                const double angle = azimuth * kDeg2Rad;
                dl->PathLineTo(ImVec2(
                    (float)(cx + R * std::sin(angle)),
                    (float)(cy - R * std::cos(angle))));
            }
            dl->PathFillConvex(
                with_alpha(mode_color, selected ? 30 : 18));

            for (const double edge : {sector_start, sector_end}) {
                const double angle =
                    (edge + ship.heading_deg) * kDeg2Rad;
                const ImVec2 end(
                    (float)(cx + R * std::sin(angle)),
                    (float)(cy - R * std::cos(angle)));
                constexpr int kDashCount = 18;
                for (int dash = 0; dash < kDashCount; dash += 2) {
                    const float t0 =
                        static_cast<float>(dash) / kDashCount;
                    const float t1 =
                        static_cast<float>(dash + 1) / kDashCount;
                    dl->AddLine(
                        ImVec2(
                            (float)cx + (end.x - (float)cx) * t0,
                            (float)cy + (end.y - (float)cy) * t0),
                        ImVec2(
                            (float)cx + (end.x - (float)cx) * t1,
                            (float)cy + (end.y - (float)cy) * t1),
                        with_alpha(mode_color, selected ? 230 : 175),
                        (selected ? 1.5f : 1.0f) * visual_scale);
                }
            }
        }

        const double label_angle =
            (face.boresight_deg + ship.heading_deg) * kDeg2Rad;
        char face_label[16];
        std::snprintf(
            face_label, sizeof face_label, "%s %s",
            face.short_name.data(), sector_mode ? "SEC" : "SRCH");
        const ImVec2 face_label_size = ImGui::CalcTextSize(face_label);
        dl->AddText(
            ImVec2((float)(cx + 0.84 * R * std::sin(label_angle))
                       - face_label_size.x * 0.5f,
                   (float)(cy - 0.84 * R * std::cos(label_angle)) - 6.0f),
            with_alpha(
                mode_color,
                selected || offline ? 235 : 130),
            face_label);
    }

    // --- azimuth spokes every 30 deg + degree labels ---
    for (int deg = 0; deg < 360; deg += 30) {
        const double a = deg * kDeg2Rad;
        const float x2 = (float)(cx + R * std::sin(a));
        const float y2 = (float)(cy - R * std::cos(a));
        dl->AddLine(ImVec2((float)cx, (float)cy), ImVec2(x2, y2),
                    theme::col_spoke(), 1.0f * visual_scale);
        char lbl[8];
        std::snprintf(lbl, sizeof lbl, "%03d", deg);
        dl->AddText(ImVec2((float)(cx + (R + 16) * std::sin(a)) - 10,
                           (float)(cy - (R + 16) * std::cos(a)) - 6),
                    theme::col_text_dim(), lbl);
    }

    // --- four independent face sweep arms ---
    for (const auto& face : faces::kDefinitions) {
        const auto face_index = static_cast<std::size_t>(face.id);
        const bool selected = face.id == selected_face_id;
        const bool offline =
            (rma_masks[face_index] & 0xFFFFu) == 0xFFFFu;
        const ImU32 sweep_color = offline
            ? theme::col_led_fault() : theme::col_sweep();
        const double angle =
            (sweep_az_deg[face_index] + ship.heading_deg) * kDeg2Rad;
        const ImVec2 end(
            (float)(cx + R * std::sin(angle)),
            (float)(cy - R * std::cos(angle)));
        if (selected) {
            dl->AddLine(
                ImVec2((float)cx, (float)cy), end,
                with_alpha(sweep_color, 45), 6.0f * visual_scale);
        }
        dl->AddLine(
            ImVec2((float)cx, (float)cy), end,
            with_alpha(sweep_color, selected ? 255 : 145),
            (selected ? 2.4f : 1.4f) * visual_scale);
        dl->AddCircleFilled(
            end, (selected ? 3.5f : 2.3f) * visual_scale,
            with_alpha(sweep_color, selected ? 255 : 180));
    }

    // --- detection blips (glow halo, SNR color, age fade) ---
    while (!blips_.empty() && now_ms - blips_.front().sim_millis > kBlipPersistMs)
        blips_.pop_front();

    for (const auto& b : blips_) {
        if (b.range_m > range_m_smooth_) continue;
        const double az_world = (b.azimuth_deg + ship.heading_deg) * kDeg2Rad;
        const double xe = b.range_m * std::sin(az_world);
        const double yn = b.range_m * std::cos(az_world);
        const ImVec2 p = enu_to_screen(cx, cy, R, xe, yn, range_m_smooth_);
        const float age = (float)(now_ms - b.sim_millis) / (float)kBlipPersistMs;
        const int base_a = (int)(255 * (1.0f - age));
        if (base_a < 12) continue;
        const ImU32 c = theme::col_snr(b.snr_db);
        dl->AddCircleFilled(
            p, 6.5f * visual_scale, with_alpha(c, base_a / 6));  // halo
        dl->AddCircleFilled(
            p, 3.5f * visual_scale, with_alpha(c, base_a / 2));  // bloom
        dl->AddCircleFilled(
            p, 2.2f * visual_scale, with_alpha(c, base_a));      // core
    }

    // --- tracks: diamond + velocity vector + fading 10-pt trail ---
    for (const auto& t : tracks) {
        const double range = std::hypot(t.x_m, t.y_m);
        const ImVec2 p = enu_to_screen(cx, cy, R, t.x_m, t.y_m, range_m_smooth_);

        // history trail
        auto it = trails_.find(t.track_id);
        if (it != trails_.end() && it->second.pts.size() > 1) {
            const auto& pts = it->second.pts;
            for (size_t i = 1; i < pts.size(); ++i) {
                const ImVec2 q1 = enu_to_screen(cx, cy, R, pts[i-1].first, pts[i-1].second, range_m_smooth_);
                const ImVec2 q2 = enu_to_screen(cx, cy, R, pts[i].first,   pts[i].second,   range_m_smooth_);
                const int a = (int)(140 * (double)i / pts.size());
                dl->AddLine(
                    q1, q2, with_alpha(theme::col_track(), a),
                    1.5f * visual_scale);
            }
        }

        // diamond symbol
        const float s = 6.0f * visual_scale;
        const ImVec2 diamond[5] = {
            {p.x, p.y - s}, {p.x + s, p.y}, {p.x, p.y + s}, {p.x - s, p.y}, {p.x, p.y - s}};
        dl->AddPolyline(
            diamond, 5, theme::col_track(), 0, 1.5f * visual_scale);

        // velocity vector (60 s prediction)
        const ImVec2 v = enu_to_screen(cx, cy, R,
            t.x_m + t.vx_mps * 60.0, t.y_m + t.vy_mps * 60.0, range_m_smooth_);
        dl->AddLine(
            p, v, with_alpha(theme::col_track(), 160),
            1.0f * visual_scale);

        // label
        char lbl[64];
        const double spd = std::sqrt(t.vx_mps*t.vx_mps + t.vy_mps*t.vy_mps);
        std::snprintf(lbl, sizeof lbl, "T%lld %.0fk %.0fm/s",
                      (long long)t.track_id, range / 1000.0, spd);
        dl->AddText(
            ImVec2(p.x + 9 * visual_scale, p.y - 6 * visual_scale),
            theme::col_text(), lbl);
    }

    // --- own-ship: crosshair + heading marker ---
    dl->AddLine(
        ImVec2((float)cx - 10 * visual_scale, (float)cy),
        ImVec2((float)cx + 10 * visual_scale, (float)cy),
        theme::col_ownship(), 1.5f * visual_scale);
    dl->AddLine(
        ImVec2((float)cx, (float)cy - 10 * visual_scale),
        ImVec2((float)cx, (float)cy + 10 * visual_scale),
        theme::col_ownship(), 1.5f * visual_scale);
    {
        const double h = ship.heading_deg * kDeg2Rad;
        dl->AddLine(ImVec2((float)cx, (float)cy),
                    ImVec2((float)(cx + 0.15 * R * std::sin(h)),
                           (float)(cy - 0.15 * R * std::cos(h))),
                    theme::col_ownship(), 2.0f * visual_scale);
    }

    // --- readouts (top-left, above the scope) ---
    char buf[96];
    const auto* selected_face = faces::find(selected_face_id);
    std::snprintf(
        buf, sizeof buf,
        "HDG %05.1f  SPD %04.1f kn  RNG %.0f km  FACE %s",
        ship.heading_deg, ship.speed_mps * 1.94384,
        range_m_smooth_ / 1000.0,
        selected_face ? selected_face->short_name.data() : "??");
    dl->AddText(ImVec2(wp.x + 4, wp.y), theme::col_text(), buf);

    // --- cursor range/bearing readout (mouse over the scope) ---
    if (ImGui::IsWindowHovered()) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        const double dx  = (double)m.x - cx;
        const double dyn = cy - (double)m.y;          // north-up display
        const double rfrac = std::hypot(dx, dyn) / R;
        if (rfrac <= 1.0) {
            double brg = std::atan2(dx, dyn) / kDeg2Rad;
            if (brg < 0.0) brg += 360.0;
            dl->AddCircle(
                m, 4.0f * visual_scale, theme::col_text_dim(), 0,
                1.0f * visual_scale);
            char cur[64];
            std::snprintf(cur, sizeof cur, "CUR %5.1f km  BRG %06.1f",
                          rfrac * range_m_smooth_ / 1000.0, brg);
            dl->AddText(ImVec2(wp.x + 4,
                               wp.y + h - ImGui::GetTextLineHeightWithSpacing()),
                        theme::col_text_dim(), cur);
        }
    }

    ImGui::End();
}

} // namespace radar::ui
