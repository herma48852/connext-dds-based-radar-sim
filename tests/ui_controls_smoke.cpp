// Logic-only ImGui smoke test for the operator controls. It renders the
// production A-scope and panels, discovers live item rectangles through the
// optional observer seam, and injects real press/hold/release events through
// ImGuiIO. No GLFW window, renderer, DDS participant, or display is required.

#include "CommandSink.hpp"
#include "DataBus.hpp"
#include "RadarFaces.hpp"
#include "ui/AScopeView.hpp"
#include "ui/Panels.hpp"
#include "ui/Theme.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>

namespace {

struct Rect {
    ImVec2 min;
    ImVec2 max;
};

class Probe final : public radar::ui::UiControlObserver {
public:
    void observe(radar::ui::UiControl control, int index,
                 ImVec2 min, ImVec2 max) override {
        rects_[{static_cast<int>(control), index}] = {min, max};
    }

    Rect get(radar::ui::UiControl control, int index = -1) const {
        const auto it = rects_.find({static_cast<int>(control), index});
        if (it == rects_.end()) {
            std::fprintf(stderr, "missing geometry for control=%d index=%d\n",
                         static_cast<int>(control), index);
            std::exit(2);
        }
        return it->second;
    }

private:
    std::map<std::pair<int, int>, Rect> rects_;
};

struct RecordedCommand {
    int32_t type;
    double center;
    double width;
    std::string params;
    int32_t priority;
    uint32_t target_face_mask;
};

class RecordingSink final : public radar::app::CommandSink {
public:
    void send(int32_t type, double center, double width,
              const char* params, int32_t priority,
              uint32_t target_face_mask) override {
        commands.push_back({
            type, center, width, params ? params : "", priority,
            radar::faces::normalize_target_mask(target_face_mask)});

        // Mirror CommandHandler's state transitions so subsequent frames
        // render the same active-state highlights as the live application.
        const uint32_t normalized_mask = commands.back().target_face_mask;
        const auto for_each_targeted_face = [&](auto&& action) {
            for (const auto& face : radar::faces::kDefinitions) {
                if ((normalized_mask & face.mask) != 0u)
                    action(static_cast<std::size_t>(face.id));
            }
        };
        switch (type) {
        case 0:
            for_each_targeted_face(
                [&](std::size_t i) { radar_modes[i] = 0; });
            break;
        case 1:
            for_each_targeted_face(
                [&](std::size_t i) { radar_modes[i] = 1; });
            break;
        case 2: self_test_requested = true; break;             // CMD_SELF_TEST
        case 3:
            reset_requested = true;
            radar_modes.fill(0);
            break;
        case 4:
            for_each_targeted_face(
                [&](std::size_t i) { degraded[i] = true; });
            break;
        case 5:
            for_each_targeted_face(
                [&](std::size_t i) { degraded[i] = false; });
            break;
        case 6: {                                              // CMD_RMA_OFFLINE
            for_each_targeted_face([&](std::size_t i) {
                if (commands.back().params == "all") {
                    rma_masks[i] = 0xFFFFu;
                } else {
                    const int rma =
                        std::atoi(commands.back().params.c_str());
                    rma_masks[i] |= (1u << rma);
                }
            });
            break;
        }
        case 7: {                                              // CMD_RMA_ONLINE
            for_each_targeted_face([&](std::size_t i) {
                if (commands.back().params == "all") {
                    rma_masks[i] = 0;
                } else {
                    const int rma =
                        std::atoi(commands.back().params.c_str());
                    rma_masks[i] &= ~(1u << rma);
                }
            });
            break;
        }
        default: break;
        }
    }

    std::vector<RecordedCommand> commands;
    std::array<int32_t, radar::faces::kFaceCount> radar_modes{};
    std::array<bool, radar::faces::kFaceCount> degraded{};
    bool self_test_requested = false;
    bool reset_requested = false;
    std::array<uint32_t, radar::faces::kFaceCount> rma_masks{};
};

class SmokeUi {
public:
    SmokeUi() {
        ImGui::CreateContext();
        ImPlot::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1800.0f, 1000.0f);
        io.DeltaTime = 1.0f / 60.0f;
        radar::ui::theme::configure_default_font(2.0f);
        io.FontGlobalScale = 1.0f;
        radar::ui::theme::apply_style(1.0f);

        // NewFrame requires a built atlas even though this test never uploads
        // or renders the resulting pixels through a graphics backend.
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

        trace_.magnitude.assign(512, 0.05f);
        trace_.magnitude[180] = 0.45f;
        trace_.face_id = selected_face_id;
        grid_.drift_db.assign(1024, 0.0f);
        grid_.face_id = selected_face_id;
        for (int i = 0; i < 12; ++i) {
            beam_history.push_back(radar::app::BeamView{
                selected_face_id, i, 35.0 + i * 2.25,
                3.0 + 11.0 * (i % 3), 0, 1, i * 10});
        }
        tracks.push_back(radar::app::TrackView{
            42, 1500.0, 0.0, 120.0, 125.0, 0.0, 0.0, 1, 84, 0});
        ship.heading_deg = 0.0;
        io.AddMousePosEvent(-1000.0f, -1000.0f);
    }

    ~SmokeUi() {
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
    }

    void frame() {
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        const float desired_font_size =
            panel_focus.active() ? 17.0f : 13.0f;
        if (std::fabs(desired_font_size - font_size_) > 0.01f) {
            radar::ui::theme::configure_default_font(
                2.0f, desired_font_size);
            io.FontGlobalScale = 1.0f;
            radar::ui::theme::apply_style(desired_font_size / 13.0f);
            unsigned char* pixels = nullptr;
            int width = 0, height = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
            font_size_ = desired_font_size;
        }
        ImGui::NewFrame();
        if (panel_focus.active() &&
            ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            panel_focus.clear();
        }

        // Exercise the original regression: the visible A-scope title changes
        // every frame and is rendered before the controls. Its hidden ID must
        // remain stable or Begin()->FocusWindow() kills a held button ActiveID.
        trace_.azimuth_deg = std::fmod(frame_number_ * 2.25, 360.0);
        trace_.elevation_deg = 3.0 + 11.0 * (frame_number_ % 3);
        trace_.face_id = selected_face_id;
        grid_.face_id = selected_face_id;
        const auto selected_index =
            static_cast<std::size_t>(selected_face_id);
        const auto scenario_index =
            static_cast<std::size_t>(selected_face_id);
        const auto render_ascope = [&](ImVec2 pos, ImVec2 size) {
            ascope_.render(
                "A-SCOPE - AMPLITUDE / RANGE", pos, size,
                trace_, io.DeltaTime, &panel_focus);
        };
        const auto render_array = [&](ImVec2 pos, ImVec2 size) {
            radar::ui::render_array_panel(
                "ARRAY FACE", pos, size, grid_,
                sink.rma_masks[selected_index], selected_face_id, sink,
                &probe, &panel_focus);
        };
        const auto render_scenarios = [&](ImVec2 pos, ImVec2 size) {
            radar::ui::render_scenario_bar(
                "SCENARIOS", pos, size, sink,
                sink.radar_modes[scenario_index],
                sink.degraded[scenario_index],
                selected_face_id, beam_formation_overlay,
                &probe, &panel_focus);
        };
        const auto render_timeline = [&](ImVec2 pos, ImVec2 size) {
            radar::ui::render_beam_timeline(
                "BEAM SCHEDULE", pos, size, beam_history,
                selected_face_id, beam_timeline_state,
                &probe, &panel_focus);
        };
        const auto render_tracks = [&](ImVec2 pos, ImVec2 size) {
            radar::ui::render_track_list(
                "TARGET TRACKS", pos, size, tracks, ship, &panel_focus);
        };

        const auto focused = panel_focus.focused();
        if (focused != radar::ui::PanelId::AScope)
            render_ascope(ImVec2(0, 0), ImVec2(1200, 380));
        if (focused != radar::ui::PanelId::ArrayFace)
            render_array(ImVec2(0, 400), ImVec2(700, 580));
        if (focused != radar::ui::PanelId::Scenarios)
            render_scenarios(ImVec2(720, 400), ImVec2(460, 580));
        if (focused != radar::ui::PanelId::BeamTimeline)
            render_timeline(ImVec2(1200, 0), ImVec2(580, 380));
        if (focused != radar::ui::PanelId::TrackList)
            render_tracks(ImVec2(1200, 400), ImVec2(580, 580));

        const ImVec2 focus_size(1800, 1000);
        if (focused == radar::ui::PanelId::AScope)
            render_ascope(ImVec2(0, 0), focus_size);
        else if (focused == radar::ui::PanelId::ArrayFace)
            render_array(ImVec2(0, 0), focus_size);
        else if (focused == radar::ui::PanelId::Scenarios)
            render_scenarios(ImVec2(0, 0), focus_size);
        else if (focused == radar::ui::PanelId::BeamTimeline)
            render_timeline(ImVec2(0, 0), focus_size);
        else if (focused == radar::ui::PanelId::TrackList)
            render_tracks(ImVec2(0, 0), focus_size);

        ImGui::Render();
        ++frame_number_;
    }

    size_t click(radar::ui::UiControl control, int index = -1) {
        const Rect r = probe.get(control, index);
        const ImVec2 center((r.min.x + r.max.x) * 0.5f,
                            (r.min.y + r.max.y) * 0.5f);
        ImGuiIO& io = ImGui::GetIO();
        const size_t before = sink.commands.size();

        io.AddMousePosEvent(center.x, center.y);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame();
        frame(); // hold for a full frame: ActiveID must survive
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame();
        io.AddMousePosEvent(-1000.0f, -1000.0f);
        frame();
        return sink.commands.size() - before;
    }

    void click_focus_toggle(const char* window_name) {
        ImGuiWindow* window = find_window(window_name);
        if (!window) {
            std::fprintf(stderr, "missing ImGui window: %s\n", window_name);
            std::exit(2);
        }
        const float title_height = ImGui::GetFrameHeight();
        const float side = std::max(16.0f, title_height - 4.0f);
        const ImVec2 center(
            window->Pos.x + window->Size.x - side * 0.5f - 4.0f,
            window->Pos.y + 2.0f + side * 0.5f);
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(center.x, center.y);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame();
        io.AddMousePosEvent(-1000.0f, -1000.0f);
        frame();
    }

    void press_escape() {
        ImGuiIO& io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiKey_Escape, true);
        frame();
        io.AddKeyEvent(ImGuiKey_Escape, false);
        frame();
    }

    Rect window_rect(const char* window_name) const {
        ImGuiWindow* window = find_window(window_name);
        if (!window) {
            std::fprintf(stderr, "missing ImGui window: %s\n", window_name);
            std::exit(2);
        }
        return {
            window->Pos,
            ImVec2(
                window->Pos.x + window->Size.x,
                window->Pos.y + window->Size.y)};
    }

    Probe probe;
    RecordingSink sink;
    int32_t selected_face_id = radar::faces::kForwardStarboard;
    bool beam_formation_overlay = false;
    radar::ui::PanelFocusState panel_focus;
    std::deque<radar::app::BeamView> beam_history;
    radar::ui::BeamTimelineState beam_timeline_state;
    std::vector<radar::app::TrackView> tracks;
    radar::app::ShipView ship{};

private:
    radar::ui::AScopeView ascope_;
    radar::app::TraceBuffer trace_;
    radar::app::ArrayGridView grid_;
    int frame_number_ = 0;
    float font_size_ = 13.0f;

    static ImGuiWindow* find_window(const char* window_name) {
        for (ImGuiWindow* candidate : GImGui->Windows) {
            if (std::string(candidate->Name).find(window_name)
                != std::string::npos) {
                return candidate;
            }
        }
        return nullptr;
    }
};

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void expect_command(SmokeUi& ui, radar::ui::UiControl control,
                    int32_t type, const char* params = "",
                    double center = 0.0, double width = 0.0,
                    int index = -1,
                    uint32_t expected_face_mask =
                        radar::faces::kForwardStarboardMask) {
    const size_t before = ui.sink.commands.size();
    check(ui.click(control, index) == 1, "control emitted exactly one command");
    if (ui.sink.commands.size() <= before)
        return;
    const RecordedCommand& cmd = ui.sink.commands.back();
    check(cmd.type == type, "command type matches control");
    check(cmd.params == params, "command parameters match control");
    check(std::fabs(cmd.center - center) < 1.0e-9,
          "command sector center matches control");
    check(std::fabs(cmd.width - width) < 1.0e-9,
          "command sector width matches control");
    check(cmd.priority == 3, "command priority remains the UI default");
    check(cmd.target_face_mask == expected_face_mask,
          "command targets the expected face set");
}

} // namespace

int main() {
    SmokeUi ui;

    radar::ui::theme::apply_style(2.0f);
    const ImGuiStyle expected_2x = ImGui::GetStyle();
    radar::ui::theme::apply_style(1.0f);
    radar::ui::theme::apply_style(2.0f);
    const ImGuiStyle& repeated_2x = ImGui::GetStyle();
    check(std::fabs(repeated_2x.FramePadding.x - expected_2x.FramePadding.x)
              < 1.0e-6f &&
          std::fabs(repeated_2x.ScrollbarSize - expected_2x.ScrollbarSize)
              < 1.0e-6f &&
          std::fabs(repeated_2x.WindowPadding.x - expected_2x.WindowPadding.x)
              < 1.0e-6f,
          "repeated monitor-scale changes do not compound ImGui dimensions");
    radar::ui::theme::apply_style(1.0f);

    // Warm up hover/window state and capture every production control rect.
    for (int i = 0; i < 4; ++i)
        ui.frame();

    check(!ui.panel_focus.active(),
          "panel focus starts in dashboard mode");
    ui.click_focus_toggle("A_SCOPE");
    check(ui.panel_focus.is_focused(radar::ui::PanelId::AScope),
          "A-scope title-bar control enters focused mode");
    const Rect focused_ascope = ui.window_rect("A_SCOPE");
    check(std::fabs(focused_ascope.min.x) < 1.0e-6f &&
              std::fabs(focused_ascope.min.y) < 1.0e-6f &&
              std::fabs(focused_ascope.max.x - 1800.0f) < 1.0e-6f &&
              std::fabs(focused_ascope.max.y - 1000.0f) < 1.0e-6f,
          "focused A-scope fills the complete client area");
    check(ImGui::GetIO().Fonts->ConfigData.Size == 1 &&
              std::fabs(
                  ImGui::GetIO().Fonts->ConfigData[0].SizePixels - 17.0f)
                  < 1.0e-6f,
          "focused panel rebuilds a crisp 17-point presenter font");
    ui.click_focus_toggle("A_SCOPE");
    check(!ui.panel_focus.active(),
          "focused A-scope title-bar control restores dashboard mode");
    const Rect restored_ascope = ui.window_rect("A_SCOPE");
    check(std::fabs(restored_ascope.max.x - 1200.0f) < 1.0e-6f &&
              std::fabs(restored_ascope.max.y - 380.0f) < 1.0e-6f,
          "contract restores the A-scope dashboard geometry");
    check(ImGui::GetIO().Fonts->ConfigData.Size == 1 &&
              std::fabs(
                  ImGui::GetIO().Fonts->ConfigData[0].SizePixels - 13.0f)
                  < 1.0e-6f,
          "contract restores the normal 13-point dashboard font");
    ui.click_focus_toggle("A_SCOPE");
    check(ui.panel_focus.is_focused(radar::ui::PanelId::AScope),
          "A-scope can re-enter focused mode");
    ui.press_escape();
    check(!ui.panel_focus.active(),
          "Escape restores the complete dashboard");

    const std::size_t live_beams_before_pause = ui.beam_history.size();
    check(ui.click(radar::ui::UiControl::BeamTimelinePause) == 0,
          "beam timeline pause is a local display control");
    check(ui.beam_timeline_state.paused() &&
              ui.beam_timeline_state.snapshot_size()
                  == live_beams_before_pause,
          "beam timeline pause captures the current live history");
    ui.beam_history.push_back(radar::app::BeamView{
        radar::faces::kForwardStarboard, 99, 45.0, 3.0, 0, 1, 990});
    ui.frame();
    check(ui.beam_timeline_state.snapshot_size()
              == live_beams_before_pause,
          "paused beam timeline remains frozen while live history advances");
    check(ui.click(radar::ui::UiControl::BeamTimelinePause) == 0,
          "beam timeline resume emits no DDS command");
    check(!ui.beam_timeline_state.paused() &&
              ui.beam_timeline_state.snapshot_size() == 0,
          "beam timeline resume returns to live history");

    // Retina uses a denser atlas without doubling logical text metrics.
    const ImGuiIO& io = ImGui::GetIO();
    check(std::fabs(io.FontGlobalScale - 1.0f) < 1.0e-6f,
          "Retina font keeps Windows-compatible logical scale");
    check(io.Fonts->ConfigData.Size == 1 &&
              std::fabs(io.Fonts->ConfigData[0].RasterizerDensity - 2.0f)
                  < 1.0e-6f,
          "Retina font atlas is rasterized at 2x density");
    check(ImGui::CalcTextSize("T0000").x < 50.0f,
          "Retina track-table text remains compact");

    // A changing visible heading must not accumulate/focus new ImGui windows.
    const int stable_window_count = GImGui->Windows.Size;
    for (int i = 0; i < 120; ++i)
        ui.frame();
    check(GImGui->Windows.Size == stable_window_count,
          "dynamic A-scope title keeps one stable ImGui window identity");

    expect_command(ui, radar::ui::UiControl::SearchMode, 0, "search");
    check(ui.sink.radar_modes[radar::faces::kForwardStarboard] == 0,
          "SEARCH MODE leaves selected face search active");

    expect_command(
        ui, radar::ui::UiControl::SectorScan, 1, "", 45.0, 30.0);
    check(ui.sink.radar_modes[radar::faces::kForwardStarboard] == 1,
          "SECTOR SCAN activates selected face sector mode");

    const size_t before_overlay = ui.sink.commands.size();
    check(ui.click(radar::ui::UiControl::BeamFormation) == 0,
          "BEAM FORMATION is a local display control");
    check(ui.beam_formation_overlay,
          "BEAM FORMATION enables the comparison overlay");
    check(ui.sink.commands.size() == before_overlay,
          "BEAM FORMATION emits no DDS command");
    check(ui.click(radar::ui::UiControl::BeamFormation) == 0,
          "BEAM FORMATION toggles locally a second time");
    check(!ui.beam_formation_overlay,
          "BEAM FORMATION disables the comparison overlay");

    expect_command(ui, radar::ui::UiControl::DegradeArray, 4);
    check(ui.sink.degraded[radar::faces::kForwardStarboard],
          "DEGRADE ARRAY sets selected-face degraded state");

    expect_command(ui, radar::ui::UiControl::RestoreArray, 5);
    check(!ui.sink.degraded[radar::faces::kForwardStarboard],
          "RESTORE ARRAY clears selected-face degraded state");

    expect_command(
        ui, radar::ui::UiControl::SelfTest, 2, "", 0.0, 0.0, -1,
        radar::faces::kAllFacesMask);
    check(ui.sink.self_test_requested, "SELF TEST request is observed");

    expect_command(
        ui, radar::ui::UiControl::ResetTracks, 3, "", 0.0, 0.0, -1,
        radar::faces::kAllFacesMask);
    check(ui.sink.reset_requested, "RESET TRACKS request is observed");
    check(std::all_of(
              ui.sink.radar_modes.begin(), ui.sink.radar_modes.end(),
              [](int32_t mode) { return mode == 0; }),
          "RESET TRACKS returns every face to search mode");

    // Cover the manual RMA hit-test in both directions.
    expect_command(ui, radar::ui::UiControl::RmaBlock, 6, "0", 0.0, 0.0, 0);
    check(ui.sink.rma_masks[radar::faces::kForwardStarboard] == 0x0001u,
          "RMA 0 selection takes selected-face block offline");
    expect_command(ui, radar::ui::UiControl::RmaBlock, 7, "0", 0.0, 0.0, 0);
    check(ui.sink.rma_masks[radar::faces::kForwardStarboard] == 0u,
          "offline RMA selection restores selected-face block online");

    // A partially degraded face first transitions to all-offline, then the
    // same control changes to ALL ONLINE and restores only that face.
    expect_command(ui, radar::ui::UiControl::RmaBlock, 6, "5", 0.0, 0.0, 5);
    check(ui.sink.rma_masks[radar::faces::kForwardStarboard] == 0x0020u,
          "RMA 5 is offline before whole-face toggle");
    expect_command(ui, radar::ui::UiControl::AllOffline, 6, "all");
    check(ui.sink.rma_masks[radar::faces::kForwardStarboard] == 0xFFFFu,
          "ALL OFFLINE darkens every RMA on the selected face");
    expect_command(ui, radar::ui::UiControl::AllOnline, 7, "all");
    check(ui.sink.rma_masks[radar::faces::kForwardStarboard] == 0u,
          "ALL ONLINE restores every RMA on the selected face");

    // Face selection is local UI state; subsequent scenarios and RMA actions
    // carry the selected face mask and leave the other three faces unchanged.
    const size_t before_face_select = ui.sink.commands.size();
    check(ui.click(
              radar::ui::UiControl::FaceSelect,
              radar::faces::kForwardPort) == 0,
          "face selector emits no DDS command");
    check(ui.selected_face_id == radar::faces::kForwardPort,
          "face selector activates Forward Port");
    check(ui.sink.commands.size() == before_face_select,
          "face selection remains display-local");

    expect_command(
        ui, radar::ui::UiControl::SectorScan, 1, "", 315.0, 30.0, -1,
        radar::faces::kForwardPortMask);
    check(ui.sink.radar_modes[radar::faces::kForwardPort] == 1 &&
              ui.sink.radar_modes[radar::faces::kForwardStarboard] == 0,
          "Forward Port sector command does not alter Forward Starboard");
    expect_command(
        ui, radar::ui::UiControl::RmaBlock, 6, "3", 0.0, 0.0, 3,
        radar::faces::kForwardPortMask);
    check(ui.sink.rma_masks[radar::faces::kForwardPort] == 0x0008u &&
              ui.sink.rma_masks[radar::faces::kForwardStarboard] == 0u,
          "Forward Port RMA outage is face-local");

    if (failures != 0) {
        std::fprintf(stderr, "ui_controls_smoke: %d failure(s)\n", failures);
        return 1;
    }
    std::printf(
        "ui_controls_smoke: PASS "
        "(panel focus/presenter mode, beam pause/resume, face selection, "
        "30-degree sector, and face-local RMA controls)\n");
    return 0;
}
