#pragma once
// UiApp: owns the GLFW window, the ImGui/ImPlot contexts and the render
// loop. The render thread is the ONLY thread that touches ImGui/GPU;
// all DDS data arrives through the DataBus (lock-free queues + stores).
// On Apple the renderer is native Metal (MetalContext) — no OpenGL.

#include <deque>

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
};

} // namespace radar::ui
