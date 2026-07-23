#pragma once
// UiApp: owns the GLFW window, the ImGui/ImPlot contexts and the render
// loop. The render thread is the ONLY thread that touches ImGui/GPU;
// all DDS data arrives through the DataBus (lock-free queues + stores).
// On Apple the renderer is native Metal (MetalContext) — no OpenGL.

#include <deque>
#include <functional>
#include <utility>

#include <GLFW/glfw3.h>

#if defined(__APPLE__)
#  include "MetalContext.hpp"
#endif

#include "../CommandConsole.hpp"
#include "../DataBus.hpp"
#include "AScopeView.hpp"
#include "BScopeView.hpp"
#include "PpiView.hpp"

namespace radar::ui {

class UiApp {
public:
    UiApp(app::DataBus& bus, app::CommandConsole& console)
        : bus_(bus), console_(console) {}
    ~UiApp();

    int run();  // blocks until the window closes; returns exit code

    // Crash-investigation knobs (apply BEFORE run()):
    void set_bscope_upload_decimation(int n) { bscope_.set_upload_decimation(n); }
    // Crash-investigation knob: undecorated window (no titlebar). 3 of 10
    // crash victims were in AppKit titlebar machinery (NSTitlebarView/
    // ContainerView, SwiftUI titlebar layout) and the ASan smear anchor was
    // a CoreText font table freed during titlebar section updates — this
    // removes that whole code path to test causality.
    void set_undecorated(bool v) { undecorated_ = v; }
    void set_stop_requested(std::function<bool()> callback) {
        stop_requested_ = std::move(callback);
    }
    void set_swap_interval(int n) {
#if defined(__APPLE__)
        (void)n; // Metal presents are display-paced by nextDrawable
#else
        swap_interval_ = n < 0 ? 0 : n;
#endif
    }

private:
    bool init();
    void shutdown();
    void update_content_scale();

    app::DataBus&        bus_;
    app::CommandConsole& console_;
    GLFWwindow*          window_ = nullptr;
#if defined(__APPLE__)
    MetalContext         metal_;
#else
    int                  swap_interval_ = 1;   // glfwSwapInterval; 2 = 30 fps
#endif

    PpiView    ppi_;
    AScopeView ascope_;
    BScopeView bscope_;

    std::deque<app::BeamView> beam_history_;   // last ~5 s of dwells
    bool show_beam_formation_ = false;         // operator-selected overlay
    bool undecorated_ = false;                 // --no-titlebar experiment
    float content_scale_ = 0.0f;
    std::function<bool()> stop_requested_;
};

} // namespace radar::ui
