#include "Panels.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

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
        static thread_local std::vector<double> xs, ys;
        xs.resize(history.size());
        ys.resize(history.size());
        const double t0 = history.front().sim_millis / 1000.0;
        for (size_t i = 0; i < history.size(); ++i) {
            xs[i] = history[i].sim_millis / 1000.0 - t0;
            ys[i] = history[i].azimuth_deg;
        }
        ImPlot::SetupAxes("t [s]", "az [deg]", 0, 0);
        ImPlot::SetupAxesLimits(xs.front(), xs.back() + 0.5, 0, 360, ImGuiCond_Always);
        ImPlot::SetNextLineStyle(ImVec4(0.35f, 1.0f, 0.55f, 1.0f), 1.5f);
        ImPlot::PlotLine("beam az", xs.data(), ys.data(), (int)xs.size());
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

void render_scenario_bar(const char* title, ImVec2 pos, ImVec2 size,
                         app::CommandConsole& console) {
    begin_panel(title, pos, size);
    // (No in-content title: the window title bar already reads
    // "SCENARIOS" — removing it keeps all seven buttons inside the
    // reduced bottom-strip height.)

    if (ImGui::Button("SEARCH MODE"))
        console.send(0, 0, 0, "search");            // CMD_SET_MODE
    if (ImGui::Button("SECTOR SCAN"))
        console.send(1, 90.0, 60.0);                 // CMD_SET_SECTOR
    ImGui::Separator();
    if (ImGui::Button("DEGRADE ARRAY"))
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
