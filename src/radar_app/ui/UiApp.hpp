#pragma once
// UiApp: owns the GLFW window, the ImGui/ImPlot contexts and the render
// loop. The render thread is the ONLY thread that touches ImGui/OpenGL;
// all DDS data arrives through the DataBus (lock-free queues + stores).

#include <deque>

#include <GLFW/glfw3.h>

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

private:
    bool init();
    void shutdown();
    void frame(float dt, int64_t now_ms);

    app::DataBus&        bus_;
    app::CommandConsole& console_;
    GLFWwindow*          window_ = nullptr;

    PpiView    ppi_;
    AScopeView ascope_;
    BScopeView bscope_;

    std::deque<app::BeamView> beam_history_;   // last ~5 s of dwells
};

} // namespace radar::ui
