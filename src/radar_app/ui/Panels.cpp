#include "Panels.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <string>

#include <implot.h>

#include "Theme.hpp"

namespace radar::ui {

namespace {
void begin_panel(const char* title, ImVec2 pos, ImVec2 size) {
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::Begin(title, nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoResize);
}

void led(ImDrawList* dl, ImVec2 center, ImU32 color, const char* label) {
    dl->AddCircleFilled(center, 5.0f, color);
    dl->AddCircle(center, 5.0f, IM_COL32(0, 0, 0, 120));
    dl->AddText(ImVec2(center.x + 9, center.y - 7), theme::col_text(), label);
}

const char* class_name(int c) {
    switch (c) {
    case 1: return "AIR";
    case 2: return "BAL";
    case 3: return "SURF";
    case 4: return "CLTR";
    default: return "UNK";
    }
}

// Element drift [dB] -> heatmap color for the ARRAY FACE pane.
ImU32 drift_color(float db) {
    if (db < -20.0f) return IM_COL32(35, 12, 14, 255);   // RMA offline: dark
    if (db < -1.0f)  return IM_COL32(200, 50, 45, 255);  // hard fail
    if (db < -0.45f) return IM_COL32(190, 140, 45, 255); // amber drift
    return IM_COL32(45, 170, 70, 255);                    // nominal
}

// Scenario button with active-state highlight: shows which scenario is
// currently in play (persistent states only; momentary actions stay plain).
bool scenario_button(const char* label, bool active) {
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.12f, 0.50f, 0.22f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.65f, 0.30f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.24f, 0.78f, 0.36f, 1.0f));
    }
    const bool clicked = ImGui::Button(label);
    if (active) ImGui::PopStyleColor(3);
    return clicked;
}
} // namespace

void render_track_list(const char* title, ImVec2 pos, ImVec2 size,
                       const std::vector<app::TrackView>& tracks,
                       const app::ShipView& ship) {
    begin_panel(title, pos, size);

    if (ImGui::BeginTable("tracks", 7,
            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY)) {
        // ID width from the rendered text itself ("T" + 4 pool digits):
        // CalcTextSize is FontGlobalScale-aware, so the column fits on
        // Retina too — the old equal-stretch share clipped the 4th digit
        // at 2x UI scale. The other six columns keep stretching.
        const float id_w = ImGui::CalcTextSize("T0000").x
                         + 2.0f * ImGui::GetStyle().CellPadding.x;
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, id_w);
        ImGui::TableSetupColumn("CLASS");
        ImGui::TableSetupColumn("RANGE");
        ImGui::TableSetupColumn("AZ");
        ImGui::TableSetupColumn("SPD");
        ImGui::TableSetupColumn("ALT");
        ImGui::TableSetupColumn("QUAL");
        ImGui::TableHeadersRow();

        auto sorted = tracks;
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
            return (a.x_m*a.x_m + a.y_m*a.y_m) < (b.x_m*b.x_m + b.y_m*b.y_m);
        });

        for (const auto& t : sorted) {
            const double range = std::hypot(t.x_m, t.y_m);
            const double az_w = std::atan2(t.x_m, t.y_m) * 180.0 / 3.14159265358979;
            double az = std::fmod(az_w - ship.heading_deg + 360.0, 360.0);
            const double spd = std::sqrt(t.vx_mps*t.vx_mps + t.vy_mps*t.vy_mps);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("T%lld", (long long)t.track_id);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(class_name(t.classification));
            ImGui::TableNextColumn(); ImGui::Text("%.1f km", range / 1000.0);
            ImGui::TableNextColumn(); ImGui::Text("%05.1f", az);
            ImGui::TableNextColumn(); ImGui::Text("%.0f", spd);
            ImGui::TableNextColumn(); ImGui::Text("%.0f", t.z_m);
            ImGui::TableNextColumn(); ImGui::Text("%d", t.quality);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void render_beam_timeline(const char* title, ImVec2 pos, ImVec2 size,
                          const std::deque<app::BeamView>& history) {
    begin_panel(title, pos, size);

    if (history.size() >= 2 && ImPlot::BeginPlot("##beamtl", ImVec2(-1, -1),
                                                 ImPlotFlags_NoMenus)) {
        static thread_local std::vector<double> xs, ys, ys2;
        xs.resize(history.size());
        ys.resize(history.size());
        ys2.resize(history.size());
        const double t0 = history.front().sim_millis / 1000.0;
        for (size_t i = 0; i < history.size(); ++i) {
            xs[i]  = history[i].sim_millis / 1000.0 - t0;
            ys[i]  = history[i].azimuth_deg;
            ys2[i] = history[i].elevation_deg;
        }
        // az on the left axis (0-360 sawtooth); el on a right Y2 axis —
        // the 3-bar raster (3/14/25 deg, one bar per 1.6 s revolution) is
        // invisible on the 0-360 scale.
        ImPlot::SetupAxes("t [s]", "az [deg]", 0, 0);
        ImPlot::SetupAxis(ImAxis_Y2, "el [deg]", ImPlotAxisFlags_AuxDefault);
        ImPlot::SetupAxesLimits(xs.front(), xs.back() + 0.5, 0, 360, ImGuiCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y2, 0, 30, ImGuiCond_Always);
        ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
        ImPlot::SetNextLineStyle(ImVec4(0.35f, 1.0f, 0.55f, 1.0f), 1.5f);
        ImPlot::PlotLine("beam az", xs.data(), ys.data(), (int)xs.size());
        ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
        ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.75f, 0.30f, 1.0f), 1.5f);
        ImPlot::PlotLine("beam el", xs.data(), ys2.data(), (int)xs.size());
        ImPlot::EndPlot();
    }
    ImGui::End();
}

void render_health_panel(const char* title, ImVec2 pos, ImVec2 size,
                         const app::HealthView& h) {
    begin_panel(title, pos, size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    float y = p.y + 10;

    const char* status_text = "NOMINAL";
    ImU32 status_col = theme::col_led_ok();
    if (h.overall_status == 1)      { status_text = "DEGRADED"; status_col = theme::col_led_warn(); }
    else if (h.overall_status == 2) { status_text = "CRITICAL"; status_col = theme::col_led_fault(); }
    else if (h.overall_status == 3) { status_text = "OFFLINE";  status_col = theme::col_led_fault(); }

    led(dl, ImVec2(p.x + 14, y), status_col, status_text);   y += 26;
    led(dl, ImVec2(p.x + 14, y), theme::col_led_ok(), "DDS BUS"); y += 26;

    ImGui::SetCursorScreenPos(ImVec2(p.x, y));
    ImGui::Text("TEMP    %5.1f C", h.temperature_c);
    ImGui::Text("FAILED  %d / %d", h.failed_element_count, h.total_elements);
    ImGui::Text("DRIFT   %5.2f dB avg", h.mean_abs_drift_db);
    ImGui::Text("RMA OFF %2d / 16", std::popcount(h.rma_mask & 0xFFFFu));

    const float frac = h.total_elements > 0
        ? (float)h.failed_element_count / h.total_elements : 0.0f;
    ImGui::ProgressBar(frac, ImVec2(-1, 0), "");
    ImGui::End();
}

void render_ship_panel(const char* title, ImVec2 pos, ImVec2 size,
                       const app::ShipView& s) {
    begin_panel(title, pos, size);
    ImGui::Text("LAT  %9.4f", s.latitude_deg);
    ImGui::Text("LON  %9.4f", s.longitude_deg);
    ImGui::Text("HDG  %6.1f deg", s.heading_deg);
    ImGui::Text("COG  %6.1f deg", s.course_deg);
    ImGui::Text("SOG  %6.1f kn", s.speed_mps * 1.94384);
    ImGui::Text("PIT  %5.1f  ROL %5.1f", s.pitch_deg, s.roll_deg);
    ImGui::End();
}

void render_array_panel(const char* title, ImVec2 pos, ImVec2 size,
                        const app::ArrayGridView& grid, uint32_t live_mask,
                        app::CommandConsole& console) {
    begin_panel(title, pos, size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = ImGui::GetContentRegionAvail().y;
    const float row_h = ImGui::GetTextLineHeightWithSpacing() + 10.0f;
    const float cell  = std::max(1.0f, std::min(w, h - row_h) / 32.0f);
    const float grid_px = cell * 32.0f;

    // 32x32 element heatmap (row-major, element i -> RMA (i/256)*4+(i%32)/8)
    const int n = static_cast<int>(grid.drift_db.size());
    for (int r = 0; r < 32; ++r) {
        for (int c = 0; c < 32; ++c) {
            const int i = r * 32 + c;
            const float d = i < n ? grid.drift_db[i] : 0.0f;
            dl->AddRectFilled(ImVec2(wp.x + c * cell,       wp.y + r * cell),
                              ImVec2(wp.x + (c + 1) * cell, wp.y + (r + 1) * cell),
                              drift_color(d));
        }
    }

    // 4x4 RMA blocks: separators; offline blocks outlined from the LIVE
    // mask (instant feedback; the DDS-fed drift heatmap lags ~1 s).
    int n_off = 0;
    for (int br = 0; br < 4; ++br) {
        for (int bc = 0; bc < 4; ++bc) {
            const int rma = br * 4 + bc;
            const ImVec2 p0(wp.x + bc * 8 * cell,       wp.y + br * 8 * cell);
            const ImVec2 p1(wp.x + (bc + 1) * 8 * cell, wp.y + (br + 1) * 8 * cell);
            const bool off = (live_mask >> rma) & 1u;
            if (off) ++n_off;
            dl->AddRect(p0, p1, off ? theme::col_led_fault() : theme::col_border(),
                        0.0f, 0, off ? 2.0f : 1.0f);
        }
    }

    // Click-to-toggle: manual hit test against the block rects (the
    // InvisibleButton version never fired — no widget IDs involved here).
    // CMD_RMA_OFFLINE = 6, CMD_RMA_ONLINE = 7 (idl CommandType).
    if (ImGui::IsWindowHovered() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        const int bc = static_cast<int>((m.x - wp.x) / (8.0f * cell));
        const int br = static_cast<int>((m.y - wp.y) / (8.0f * cell));
        if (bc >= 0 && bc < 4 && br >= 0 && br < 4) {
            const int rma = br * 4 + bc;
            const bool off = (live_mask >> rma) & 1u;
            console.send(off ? 7 : 6, 0.0, 0.0, std::to_string(rma).c_str());
        }
    }

    ImGui::SetCursorScreenPos(ImVec2(wp.x, wp.y + grid_px + 4.0f));
    ImGui::Text("RMA OFF %2d/16", n_off);
    ImGui::SameLine();
    if (ImGui::SmallButton("ALL ONLINE"))
        console.send(7, 0.0, 0.0, "all");   // CMD_RMA_ONLINE, all
    ImGui::End();
}

void render_scenario_bar(const char* title, ImVec2 pos, ImVec2 size,
                         app::CommandConsole& console,
                         int32_t radar_mode, bool degraded) {
    begin_panel(title, pos, size);
    // Persistent scenario states highlighted so the operator can see what
    // is in play; the app boots in search mode (radar_mode == 0).
    if (scenario_button("SEARCH MODE", radar_mode == 0))
        console.send(0, 0, 0, "search");            // CMD_SET_MODE
    if (scenario_button("SECTOR SCAN", radar_mode == 1))
        console.send(1, 90.0, 60.0);                 // CMD_SET_SECTOR
    ImGui::Separator();
    if (scenario_button("DEGRADE ARRAY", degraded))
        console.send(4);                             // CMD_DEGRADE_ARRAY
    if (ImGui::Button("RESTORE ARRAY"))
        console.send(5);                             // CMD_RESTORE_ARRAY
    ImGui::Separator();
    if (ImGui::Button("SELF TEST"))
        console.send(2);                             // CMD_SELF_TEST
    if (ImGui::Button("RESET TRACKS"))
        console.send(3);                             // CMD_RESET

    ImGui::End();
}

} // namespace radar::ui
