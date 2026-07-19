#pragma once
// PPI (Plan Position Indicator): circular 360 deg display.
//  - range rings with distance labels, azimuth spokes every 30 deg
//  - sweep arm with fading motion-blur trail
//  - glowing SNR-coded detection blips with ~6 s persistence
//  - cyan track symbols with velocity vectors and 10-position history trails
//  - own-ship crosshair + heading marker + course/speed readout
//  - mouse-wheel zoom with smooth interpolation

#include <deque>
#include <unordered_map>
#include <vector>

#include <imgui.h>

#include "../DataBus.hpp"

namespace radar::ui {

class PpiView {
public:
    void add_blip(const app::BlipView& b);
    void update_track_trail(int64_t track_id, double x_east, double y_north);
    void prune_tracks(const std::vector<app::TrackView>& live);

    void render(const char* title, ImVec2 pos, ImVec2 size,
                const std::vector<app::TrackView>& tracks,
                const app::ShipView& ship,
                double sweep_az_deg,
                int64_t now_ms, float dt);

    double display_range_m() const { return range_m_smooth_; }

private:
    struct Trail { std::deque<std::pair<double,double>> pts; }; // ENU, max 10

    std::deque<app::BlipView> blips_;                  // pooled ring (max 2048)
    std::unordered_map<int64_t, Trail> trails_;

    double range_m_target_ = 40000.0;                  // wheel-selected
    double range_m_smooth_ = 40000.0;                  // interpolated
};

} // namespace radar::ui
