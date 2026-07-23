// Logic-only ImGui smoke test for the operator controls. It renders the
// production A-scope and panels, discovers live item rectangles through the
// optional observer seam, and injects real press/hold/release events through
// ImGuiIO. No GLFW window, renderer, DDS participant, or display is required.

#include "CommandSink.hpp"
#include "DataBus.hpp"
#include "ui/AScopeView.hpp"
#include "ui/Panels.hpp"
#include "ui/Theme.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
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
};

class RecordingSink final : public radar::app::CommandSink {
public:
    void send(int32_t type, double center, double width,
              const char* params, int32_t priority) override {
        commands.push_back({type, center, width, params ? params : "", priority});

        // Mirror CommandHandler's state transitions so subsequent frames
        // render the same active-state highlights as the live application.
        switch (type) {
        case 0: radar_mode = 0; break;                         // CMD_SET_MODE
        case 1: radar_mode = 1; break;                         // CMD_SET_SECTOR
        case 2: self_test_requested = true; break;             // CMD_SELF_TEST
        case 3: reset_requested = true; radar_mode = 0; break; // CMD_RESET
        case 4: degraded = true; break;                        // CMD_DEGRADE_ARRAY
        case 5: degraded = false; break;                       // CMD_RESTORE_ARRAY
        case 6: {                                              // CMD_RMA_OFFLINE
            const int rma = std::atoi(commands.back().params.c_str());
            rma_mask |= (1u << rma);
            break;
        }
        case 7: {                                              // CMD_RMA_ONLINE
            if (commands.back().params == "all") {
                rma_mask = 0;
            } else {
                const int rma = std::atoi(commands.back().params.c_str());
                rma_mask &= ~(1u << rma);
            }
            break;
        }
        default: break;
        }
    }

    std::vector<RecordedCommand> commands;
    int32_t radar_mode = 0;
    bool degraded = false;
    bool self_test_requested = false;
    bool reset_requested = false;
    uint32_t rma_mask = 0;
};

class SmokeUi {
public:
    SmokeUi() {
        ImGui::CreateContext();
        ImPlot::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1200.0f, 1000.0f);
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
        grid_.drift_db.assign(1024, 0.0f);
        io.AddMousePosEvent(-1000.0f, -1000.0f);
    }

    ~SmokeUi() {
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
    }

    void frame() {
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();

        // Exercise the original regression: the visible A-scope title changes
        // every frame and is rendered before the controls. Its hidden ID must
        // remain stable or Begin()->FocusWindow() kills a held button ActiveID.
        trace_.azimuth_deg = std::fmod(frame_number_ * 2.25, 360.0);
        trace_.elevation_deg = 3.0 + 11.0 * (frame_number_ % 3);
        ascope_.render("A-SCOPE - AMPLITUDE / RANGE", ImVec2(0, 0),
                       ImVec2(1200, 380), trace_, io.DeltaTime);

        radar::ui::render_array_panel(
            "ARRAY FACE", ImVec2(0, 400), ImVec2(700, 580), grid_,
            sink.rma_mask, sink, &probe);
        radar::ui::render_scenario_bar(
            "SCENARIOS", ImVec2(720, 400), ImVec2(460, 580), sink,
            sink.radar_mode, sink.degraded, beam_formation_overlay, &probe);

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

    Probe probe;
    RecordingSink sink;
    bool beam_formation_overlay = false;

private:
    radar::ui::AScopeView ascope_;
    radar::app::TraceBuffer trace_;
    radar::app::ArrayGridView grid_;
    int frame_number_ = 0;
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
                    int index = -1) {
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
}

} // namespace

int main() {
    SmokeUi ui;

    // Warm up hover/window state and capture every production control rect.
    for (int i = 0; i < 4; ++i)
        ui.frame();

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
    check(ui.sink.radar_mode == 0, "SEARCH MODE leaves search active");

    expect_command(ui, radar::ui::UiControl::SectorScan, 1, "", 90.0, 60.0);
    check(ui.sink.radar_mode == 1, "SECTOR SCAN activates sector mode");

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
    check(ui.sink.degraded, "DEGRADE ARRAY sets degraded state");

    expect_command(ui, radar::ui::UiControl::RestoreArray, 5);
    check(!ui.sink.degraded, "RESTORE ARRAY clears degraded state");

    expect_command(ui, radar::ui::UiControl::SelfTest, 2);
    check(ui.sink.self_test_requested, "SELF TEST request is observed");

    expect_command(ui, radar::ui::UiControl::ResetTracks, 3);
    check(ui.sink.reset_requested, "RESET TRACKS request is observed");
    check(ui.sink.radar_mode == 0, "RESET TRACKS returns to search mode");

    // Cover the manual RMA hit-test in both directions.
    expect_command(ui, radar::ui::UiControl::RmaBlock, 6, "0", 0.0, 0.0, 0);
    check(ui.sink.rma_mask == 0x0001u, "RMA 0 selection takes block offline");
    expect_command(ui, radar::ui::UiControl::RmaBlock, 7, "0", 0.0, 0.0, 0);
    check(ui.sink.rma_mask == 0u, "offline RMA selection restores block online");

    // Establish a nonzero mask through the real grid, then verify the standard
    // SmallButton path that was broken alongside all scenario buttons.
    expect_command(ui, radar::ui::UiControl::RmaBlock, 6, "5", 0.0, 0.0, 5);
    check(ui.sink.rma_mask == 0x0020u, "RMA 5 is offline before ALL ONLINE");
    expect_command(ui, radar::ui::UiControl::AllOnline, 7, "all");
    check(ui.sink.rma_mask == 0u, "ALL ONLINE clears the complete RMA mask");

    if (failures != 0) {
        std::fprintf(stderr, "ui_controls_smoke: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("ui_controls_smoke: PASS (all scenario, RMA, and ALL ONLINE controls)\n");
    return 0;
}
