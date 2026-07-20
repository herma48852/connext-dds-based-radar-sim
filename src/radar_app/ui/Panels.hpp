#pragma once
// Bottom-strip panels: track list, beam schedule timeline, system health,
// ship position/status, and the scenario (SystemCommand) button bar.

#include <deque>
#include <vector>

#include <imgui.h>

#include "../CommandConsole.hpp"
#include "../DataBus.hpp"

namespace radar::ui {

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
// clicking an RMA toggles it offline/online (via the CommandConsole).
void render_array_panel(const char* title, ImVec2 pos, ImVec2 size,
                        const app::ArrayGridView& grid,
                        app::CommandConsole& console);

void render_scenario_bar(const char* title, ImVec2 pos, ImVec2 size,
                         app::CommandConsole& console);

} // namespace radar::ui
