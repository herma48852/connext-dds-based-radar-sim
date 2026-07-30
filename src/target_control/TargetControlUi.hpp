#pragma once

#include <deque>
#include <functional>
#include <map>
#include <string>
#include <utility>

#include <GLFW/glfw3.h>

#if defined(__APPLE__)
#  include "MetalContext.hpp"
#endif

#include "TargetControlClient.hpp"

namespace target_control {

class TargetControlUi {
public:
    explicit TargetControlUi(TargetControlClient& client)
        : client_(client) {}
    ~TargetControlUi();

    int run();
    void set_stop_requested(std::function<bool()> callback) {
        stop_requested_ = std::move(callback);
    }

private:
    bool init();
    void shutdown();
    void update_content_scale();
    void render();

    TargetControlClient& client_;
    GLFWwindow* window_ = nullptr;
#if defined(__APPLE__)
    radar::ui::MetalContext metal_;
#endif
    float content_scale_ = 0.0f;
    std::map<std::string, int32_t> target_counts_;
    std::deque<std::string> event_log_;
    std::function<bool()> stop_requested_;
};

} // namespace target_control
