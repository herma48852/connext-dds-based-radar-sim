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
    return (c & IM_COL32_A_MASK) | ((ImU32)a << IM_COL32_A_SHIFT);
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
                     double sweep_az_deg,
                     int64_t now_ms, float dt) {
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::Begin(title, nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoResize);

    // --- smooth zoom (mouse wheel over the scope) ---
    if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0.0f) {
        range_m_target_ *= (ImGui::GetIO().MouseWheel > 0 ? 0.9 : 1.1);
        range_m_target_ = std::clamp(range_m_target_, 10000.0, 100000.0);
    }
    range_m_smooth_ += (range_m_target_ - range_m_smooth_) * std::min(1.0f, dt * 8.0f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();
    const double cx = wp.x + ws.x * 0.5;
    const double cy = wp.y + ws.y * 0.54;
    const double R  = std::min(ws.x, ws.y) * 0.44;

    dl->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), theme::col_bg());

    // --- range rings + labels ---
    for (int i = 1; i <= 4; ++i) {
        const float r = (float)(R * i / 4.0);
        dl->AddCircle(ImVec2((float)cx, (float)cy), r, theme::col_ring(), 128, 1.0f);
        char lbl[32];
        std::snprintf(lbl, sizeof lbl, "%.0f km", range_m_smooth_ * i / 4.0 / 1000.0);
        dl->AddText(ImVec2((float)cx + 4, (float)(cy - r) - 14),
                    theme::col_text_dim(), lbl);
    }

    // --- azimuth spokes every 30 deg + degree labels ---
    for (int deg = 0; deg < 360; deg += 30) {
        const double a = deg * kDeg2Rad;
        const float x2 = (float)(cx + R * std::sin(a));
        const float y2 = (float)(cy - R * std::cos(a));
        dl->AddLine(ImVec2((float)cx, (float)cy), ImVec2(x2, y2),
                    theme::col_spoke(), 1.0f);
        char lbl[8];
        std::snprintf(lbl, sizeof lbl, "%03d", deg);
        dl->AddText(ImVec2((float)(cx + (R + 16) * std::sin(a)) - 10,
                           (float)(cy - (R + 16) * std::cos(a)) - 6),
                    theme::col_text_dim(), lbl);
    }

    // --- sweep trail (motion blur): fading arc behind the beam ---
    const double sweep_world = sweep_az_deg + ship.heading_deg;
    for (int i = 0; i < 48; ++i) {
        const double a1 = (sweep_world - i * 0.35) * kDeg2Rad;
        const double a2 = (sweep_world - (i + 1) * 0.35) * kDeg2Rad;
        const int alpha = (int)(110 * (1.0 - i / 48.0));
        dl->AddLine(ImVec2((float)(cx + R * std::sin(a1)), (float)(cy - R * std::cos(a1))),
                    ImVec2((float)(cx + R * std::sin(a2)), (float)(cy - R * std::cos(a2))),
                    with_alpha(theme::col_sweep(), alpha), 3.0f);
    }
    // bright sweep arm
    {
        const double a = sweep_world * kDeg2Rad;
        dl->AddLine(ImVec2((float)cx, (float)cy),
                    ImVec2((float)(cx + R * std::sin(a)), (float)(cy - R * std::cos(a))),
                    theme::col_sweep(), 2.0f);
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
        dl->AddCircleFilled(p, 7.0f, with_alpha(c, base_a / 8));  // halo
        dl->AddCircleFilled(p, 3.5f, with_alpha(c, base_a / 2));  // bloom
        dl->AddCircleFilled(p, 1.8f, with_alpha(c, base_a));      // core
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
                dl->AddLine(q1, q2, with_alpha(theme::col_track(), a), 1.5f);
            }
        }

        // diamond symbol
        const float s = 6.0f;
        const ImVec2 diamond[5] = {
            {p.x, p.y - s}, {p.x + s, p.y}, {p.x, p.y + s}, {p.x - s, p.y}, {p.x, p.y - s}};
        dl->AddPolyline(diamond, 5, theme::col_track(), 0, 1.5f);

        // velocity vector (60 s prediction)
        const ImVec2 v = enu_to_screen(cx, cy, R,
            t.x_m + t.vx_mps * 60.0, t.y_m + t.vy_mps * 60.0, range_m_smooth_);
        dl->AddLine(p, v, with_alpha(theme::col_track(), 160), 1.0f);

        // label
        char lbl[64];
        const double spd = std::sqrt(t.vx_mps*t.vx_mps + t.vy_mps*t.vy_mps);
        std::snprintf(lbl, sizeof lbl, "T%lld %.0fk %.0fm/s",
                      (long long)t.track_id, range / 1000.0, spd);
        dl->AddText(ImVec2(p.x + 9, p.y - 6), theme::col_text(), lbl);
    }

    // --- own-ship: crosshair + heading marker ---
    dl->AddLine(ImVec2((float)cx - 10, (float)cy), ImVec2((float)cx + 10, (float)cy),
                theme::col_ownship(), 1.5f);
    dl->AddLine(ImVec2((float)cx, (float)cy - 10), ImVec2((float)cx, (float)cy + 10),
                theme::col_ownship(), 1.5f);
    {
        const double h = ship.heading_deg * kDeg2Rad;
        dl->AddLine(ImVec2((float)cx, (float)cy),
                    ImVec2((float)(cx + 0.15 * R * std::sin(h)),
                           (float)(cy - 0.15 * R * std::cos(h))),
                    theme::col_ownship(), 2.0f);
    }

    // --- readouts ---
    char buf[96];
    std::snprintf(buf, sizeof buf, "HDG %05.1f  SPD %04.1f kn  RNG %.0f km",
                  ship.heading_deg, ship.speed_mps * 1.94384, range_m_smooth_ / 1000.0);
    dl->AddText(ImVec2(wp.x + 12, wp.y + ws.y - 22), theme::col_text(), buf);

    ImGui::End();
}

} // namespace radar::ui
