#pragma once
// Bottom-strip panels: track list, beam schedule timeline, system health,
// ship position/status, and the scenario/action button bar. Most actions
// publish SystemCommand; BEAM FORMATION is a local display toggle.

#include <deque>
#include <vector>

#include <imgui.h>

#include "../CommandSink.hpp"
#include "../DataBus.hpp"

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
    AllOnline,
    RmaBlock
};

class UiControlObserver {
public:
    virtual ~UiControlObserver() = default;
    virtual void observe(UiControl control, int index,
                         ImVec2 min, ImVec2 max) = 0;
};

void render_track_list(const char* title, ImVec2 pos, ImVec2 size,
                       const std::vector<app::TrackView>& tracks,
                       const app::ShipView& ship);

void render_beam_timeline(const char* title, ImVec2 pos, ImVec2 size,
                          const std::deque<app::BeamView>& history);

void render_health_panel(const char* title, ImVec2 pos, ImVec2 size,
                         const app::HealthView& health);

void render_ship_panel(const char* title, ImVec2 pos, ImVec2 size,
                       const app::ShipView& ship);

// ARRAY FACE: 32x32 element drift heatmap with the 4x4 RMA block grid;
// clicking an RMA toggles it offline/online through the command sink.
// live_mask = DataBus::rma_offline_mask snapshot (instant feedback; the
// drift heatmap itself stays DDS-fed via CalibrationStatus).
void render_array_panel(const char* title, ImVec2 pos, ImVec2 size,
                        const app::ArrayGridView& grid, uint32_t live_mask,
                        app::CommandSink& commands,
                        UiControlObserver* observer = nullptr);

void render_scenario_bar(const char* title, ImVec2 pos, ImVec2 size,
                         app::CommandSink& commands,
                         int32_t radar_mode, bool degraded,
                         bool& beam_formation_overlay,
                         UiControlObserver* observer = nullptr);

} // namespace radar::ui
