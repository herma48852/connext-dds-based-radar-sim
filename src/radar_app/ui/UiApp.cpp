#include "UiApp.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>

#include <imgui.h>
#include <implot.h>
#include <backends/imgui_impl_glfw.h>
#if !defined(__APPLE__)
#include <backends/imgui_impl_opengl3.h>
#endif

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
    // Native Metal renderer: NO OpenGL context anywhere in the process.
    // Apple's deprecated GL->Metal shim was the prime crash suspect.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
#else
    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#if defined(_WIN32)
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
#endif
#endif
    // Crash-investigation knob (--no-titlebar): strip AppKit titlebar code.
    if (undecorated_)
        glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);

    int window_width = 1800;
    int window_height = 1100;
#if defined(_WIN32)
    int work_x = 0, work_y = 0, work_width = 1920, work_height = 1080;
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    if (monitor) {
        glfwGetMonitorWorkarea(
            monitor, &work_x, &work_y, &work_width, &work_height);
        window_width = std::min(
            {window_width, work_width, std::max(800, work_width * 95 / 100)});
        window_height = std::min(
            {window_height, work_height, std::max(600, work_height * 90 / 100)});
    }
#endif
    window_ = glfwCreateWindow(window_width, window_height,
                               "AESA Radar Console - SPY-6 class",
                               nullptr, nullptr);
    if (!window_) {
        std::cerr << "GLFW window creation failed\n";
        return false;
    }
#if defined(_WIN32)
    glfwSetWindowPos(window_, work_x + (work_width - window_width) / 2,
                     work_y + (work_height - window_height) / 2);
#endif
#if !defined(__APPLE__)
    glfwMakeContextCurrent(window_);
    glfwSwapInterval(swap_interval_); // vsync: 1 = 60 FPS cap, 2 = 30 FPS
#endif

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // keep the demo layout deterministic

    update_content_scale();

#if defined(__APPLE__)
    ImGui_ImplGlfw_InitForOther(window_, true);
    if (!metal_.init(window_)) {
        std::cerr << "Metal init failed\n";
        return false;
    }
    bscope_.init_texture(metal_);
#else
    ImGui_ImplGlfw_InitForOpenGL(window_, true);
    ImGui_ImplOpenGL3_Init(glsl_version);
    bscope_.init_gl();
#endif
    return true;
}

void UiApp::update_content_scale() {
    float sx = 1.0f, sy = 1.0f;
    glfwGetWindowContentScale(window_, &sx, &sy);
    if (!(sx >= 1.0f && sx <= 4.0f) || !std::isfinite(sx))
        sx = 1.0f;
    if (std::fabs(sx - content_scale_) < 0.01f)
        return;
    content_scale_ = sx;
    ImGui::GetIO().FontGlobalScale = sx;
    theme::apply_style(sx);
}

void UiApp::shutdown() {
    if (!window_) return;
#if defined(__APPLE__)
    bscope_.release_texture(metal_);
    metal_.shutdown(); // ImGui_ImplMetal_Shutdown + device/queue/layer
#else
    bscope_.release_gl(); // delete GL objects while the context is current
    ImGui_ImplOpenGL3_Shutdown();
#endif
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
        if (stop_requested_ && stop_requested_()) {
            glfwSetWindowShouldClose(window_, GLFW_TRUE);
            continue;
        }
        update_content_scale();

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
#if defined(__APPLE__)
        if (!metal_.begin_frame()) {   // minimized: pump events, skip ImGui
            glfwWaitEventsTimeout(0.05);
            continue;
        }
        metal_.new_frame();            // ImGui_ImplMetal_NewFrame
#else
        ImGui_ImplOpenGL3_NewFrame();
#endif
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

        // bottom strip: 6 panes. Widths tuned so text fits at 2x UI scale
        // (health/ship need ~15%, scenario buttons ~14%, array grid ~16%).
        const float y0 = scope_h;
        const float w1 = W * 0.22f, w2 = W * 0.18f, w3 = W * 0.15f,
                    w4 = W * 0.15f, w5 = W * 0.16f,
                    w6 = W - w1 - w2 - w3 - w4 - w5;
        render_track_list("TARGET TRACKS", ImVec2(0, y0), ImVec2(w1, panel_h),
                          tracks, ship);
        render_beam_timeline("BEAM SCHEDULE", ImVec2(w1, y0), ImVec2(w2, panel_h),
                             beam_history_);
        render_health_panel("SYSTEM HEALTH", ImVec2(w1 + w2, y0),
                            ImVec2(w3, panel_h), health);
        render_ship_panel("SHIP POSITION", ImVec2(w1 + w2 + w3, y0),
                          ImVec2(w4, panel_h), ship);
        render_array_panel("ARRAY FACE", ImVec2(w1 + w2 + w3 + w4, y0),
                           ImVec2(w5, panel_h), bus_.array_grid(),
                           bus_.rma_offline_mask.load(), console_);
        render_scenario_bar("SCENARIOS", ImVec2(w1 + w2 + w3 + w4 + w5, y0),
                            ImVec2(w6, panel_h), console_,
                            bus_.radar_mode.load(), bus_.degrade_array.load());

        ImGui::Render();
#if defined(__APPLE__)
        metal_.render(); // encode + presentDrawable + commit
#else
        int fb_w, fb_h;
        glfwGetFramebufferSize(window_, &fb_w, &fb_h);
        glViewport(0, 0, fb_w, fb_h);
        glClearColor(0.04f, 0.04f, 0.06f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window_);
#endif
    }
    return 0;
}

} // namespace radar::ui
