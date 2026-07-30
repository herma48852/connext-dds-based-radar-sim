#pragma once
// Bottom-strip panels: track list, beam schedule timeline, system health,
// ship position/status, and the scenario/action button bar. Most actions
// publish SystemCommand; BEAM FORMATION is a local display toggle.

#include <deque>
#include <vector>

#include <imgui.h>

#include "../CommandSink.hpp"
#include "../DataBus.hpp"
#include "PanelFocus.hpp"

namespace radar::ui {

// Optional geometry probe used by the logic-only ImGui smoke test. Production
// passes nullptr, so it adds no state or behavior to the live UI.
enum class UiControl {
    SearchMode,
    SectorScan,
    BeamFormation,
    DegradeArray,
    RestoreArray,
    SelfTest,
    ResetTracks,
    BeamTimelinePause,
    AllOnline,
    AllOffline,
    FaceSelect,
    RmaBlock
};

class UiControlObserver {
public:
    virtual ~UiControlObserver() = default;
    virtual void observe(UiControl control, int index,
                         ImVec2 min, ImVec2 max) = 0;
};

class BeamTimelineState {
public:
    bool paused() const noexcept { return paused_; }
    std::size_t snapshot_size() const noexcept {
        return snapshot_.size();
    }

    void toggle(const std::deque<app::BeamView>& live_history) {
        if (paused_) {
            paused_ = false;
            snapshot_.clear();
        } else {
            snapshot_ = live_history;
            paused_ = true;
        }
    }

    const std::deque<app::BeamView>& displayed_history(
            const std::deque<app::BeamView>& live_history) const noexcept {
        return paused_ ? snapshot_ : live_history;
    }

private:
    bool paused_{false};
    std::deque<app::BeamView> snapshot_;
};

void render_track_list(const char* title, ImVec2 pos, ImVec2 size,
                       const std::vector<app::TrackView>& tracks,
                       const app::ShipView& ship,
                       PanelFocusState* focus = nullptr);

void render_beam_timeline(const char* title, ImVec2 pos, ImVec2 size,
                          const std::deque<app::BeamView>& history,
                          int32_t selected_face_id,
                          BeamTimelineState& state,
                          UiControlObserver* observer = nullptr,
                          PanelFocusState* focus = nullptr);

void render_health_panel(const char* title, ImVec2 pos, ImVec2 size,
                         const app::HealthView& health,
                         PanelFocusState* focus = nullptr);

void render_ship_panel(const char* title, ImVec2 pos, ImVec2 size,
                       const app::ShipView& ship,
                       PanelFocusState* focus = nullptr);

// ARRAY FACE: 32x32 element drift heatmap with the 4x4 RMA block grid;
// clicking an RMA toggles it offline/online through the command sink.
// live_mask = DataBus::rma_offline_mask snapshot (instant feedback; the
// drift heatmap itself stays DDS-fed via CalibrationStatus).
void render_array_panel(const char* title, ImVec2 pos, ImVec2 size,
                        const app::ArrayGridView& grid, uint32_t live_mask,
                        int32_t& selected_face_id,
                        app::CommandSink& commands,
                        UiControlObserver* observer = nullptr,
                        PanelFocusState* focus = nullptr);

void render_scenario_bar(const char* title, ImVec2 pos, ImVec2 size,
                         app::CommandSink& commands,
                         int32_t radar_mode, bool degraded,
                         int32_t selected_face_id,
                         bool& beam_formation_overlay,
                         UiControlObserver* observer = nullptr,
                         PanelFocusState* focus = nullptr);

} // namespace radar::ui
