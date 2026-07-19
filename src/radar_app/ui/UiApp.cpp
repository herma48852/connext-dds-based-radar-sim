#include "UiApp.hpp"

#include <algorithm>
#include <iostream>

#include <imgui.h>
#include <implot.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include "Panels.hpp"
#include "SimClock.hpp"
#include "Theme.hpp"

namespace radar::ui {

UiApp::~UiApp() { shutdown(); }

bool UiApp::init() {
    if (!glfwInit()) {
        std::cerr << "GLFW init failed\n";
        return false;
    }

#if defined(__APPLE__)
    const char* glsl_version = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#endif

    window_ = glfwCreateWindow(1800, 1100, "AESA Radar Console - SPY-6 class",
                               nullptr, nullptr);
    if (!window_) {
        std::cerr << "GLFW window creation failed\n";
        return false;
    }
    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1); // vsync: 60 FPS cap

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // keep the demo layout deterministic

    float sx = 1.0f, sy = 1.0f;
    glfwGetWindowContentScale(window_, &sx, &sy);
    // Guard against a bogus 0/NaN content scale (reported on some macOS
    // multi-monitor moves): a zero font scale crashes ImGui's atlas builder.
    if (!(sx >= 1.0f && sx <= 4.0f)) sx = 1.0f;
    io.FontGlobalScale = sx;              // crisp text on Retina/HiDPI
    theme::apply_style(sx);

    ImGui_ImplGlfw_InitForOpenGL(window_, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    bscope_.init_gl();
    return true;
}

void UiApp::shutdown() {
    if (!window_) return;
    bscope_.release_gl(); // delete GL objects while the context is current
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window_);
    glfwTerminate();
    window_ = nullptr;
}

int UiApp::run() {
    if (!init()) return 1;

    while (!glfwWindowShouldClose(window_)) {
        glfwPollEvents();

        const float dt = ImGui::GetIO().DeltaTime > 0.0f
            ? ImGui::GetIO().DeltaTime : 1.0f / 60.0f;
        const int64_t now_ms = SimClock::sim_millis();

        // ---- drain DDS-fed queues (lock-free; render thread never blocks) ----
        bus_.detection_blips.drain([&](const app::BlipView& b) {
            ppi_.add_blip(b);
            bscope_.splat(b);
        });
        bus_.beam_commands.drain([&](const app::BeamView& b) {
            if (beam_history_.size() >= 240) beam_history_.pop_front();
            beam_history_.push_back(b);
        });

        const auto tracks = bus_.tracks();
        for (const auto& t : tracks)
            ppi_.update_track_trail(t.track_id, t.x_m, t.y_m);
        ppi_.prune_tracks(tracks);

        const auto ship   = bus_.ship_display(); // via HMI-UI's DDS reader
        const auto health = bus_.health();
        const auto trace  = bus_.trace();

        // ---- render ----
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const float W = vp->Size.x, H = vp->Size.y;
        // Bottom strip sized to its content at the active UI scale; the
        // scopes are guaranteed the majority of the window. (The old
        // formula multiplied by an ALREADY-scaled WindowPadding AND by
        // FontGlobalScale, so on Retina it produced 1110pt of panels on
        // an 1100pt window — the scopes were crushed to ~100px.)
        const float ui_scale = ImGui::GetIO().FontGlobalScale;
        const float panel_h = std::clamp(240.0f * ui_scale,
                                         H * 0.27f, H * 0.5f);
        const float scope_h = H - panel_h;
        const float ppi_w = W * 0.48f;
        const float right_w = W - ppi_w;

        ppi_.render("PPI - PLAN POSITION INDICATOR",
                    ImVec2(0, 0), ImVec2(ppi_w, scope_h),
                    tracks, ship, bus_.current_beam_az_deg.load(), now_ms, dt);

        ascope_.render("A-SCOPE - AMPLITUDE / RANGE",
                       ImVec2(ppi_w, 0), ImVec2(right_w, scope_h * 0.45f),
                       trace, dt);

        bscope_.render("B-SCOPE - RANGE / AZIMUTH",
                       ImVec2(ppi_w, scope_h * 0.45f),
                       ImVec2(right_w, scope_h * 0.55f),
                       tracks, ship,
                       bus_.radar_mode.load() == 1,
                       bus_.sector_center_deg.load(),
                       bus_.sector_width_deg.load(), dt);

        // bottom strip
        const float y0 = scope_h;
        // Wider health/ship panels: at 2x UI scale their text was
        // clipped by the old 15% widths ("DRIFT ... dB avg", "PIT/ROL").
        const float w1 = W * 0.26f, w2 = W * 0.22f, w3 = W * 0.18f,
                    w4 = W * 0.18f, w5 = W - w1 - w2 - w3 - w4;
        render_track_list("TARGET TRACKS", ImVec2(0, y0), ImVec2(w1, panel_h),
                          tracks, ship);
        render_beam_timeline("BEAM SCHEDULE", ImVec2(w1, y0), ImVec2(w2, panel_h),
                             beam_history_);
        render_health_panel("SYSTEM HEALTH", ImVec2(w1 + w2, y0),
                            ImVec2(w3, panel_h), health);
        render_ship_panel("SHIP POSITION", ImVec2(w1 + w2 + w3, y0),
                          ImVec2(w4, panel_h), ship);
        render_scenario_bar("SCENARIOS", ImVec2(w1 + w2 + w3 + w4, y0),
                            ImVec2(w5, panel_h), console_);

        ImGui::Render();
        int fb_w, fb_h;
        glfwGetFramebufferSize(window_, &fb_w, &fb_h);
        glViewport(0, 0, fb_w, fb_h);
        glClearColor(0.04f, 0.04f, 0.06f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window_);
    }
    return 0;
}

} // namespace radar::ui
