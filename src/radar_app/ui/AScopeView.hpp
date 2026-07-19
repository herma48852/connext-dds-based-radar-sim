#pragma once
// A-scope: amplitude vs. range, CRT phosphor style.
//  - green phosphor trace with exponential persistence decay
//  - subtle grid with range markers
//  - glow pass + crisp core pass (variable apparent thickness)
//  - peak-hold triangles at threshold crossings
//  - range-gate bracket around the strongest return

#include <vector>

#include <imgui.h>

#include "../DataBus.hpp"

namespace radar::ui {

class AScopeView {
public:
    void render(const char* title, ImVec2 pos, ImVec2 size,
                const app::TraceBuffer& trace, float dt);

private:
    static constexpr float kDisplayFullScale = 0.6f;   // linear amplitude
    static constexpr float kDetectThreshold  = 0.26f;  // mirrors CFAR

    std::vector<float> phosphor_;   // persistence buffer, same size as trace
    double range_max_m_ = 100000.0;
    double az_deg_ = 0.0, el_deg_ = 0.0;
};

} // namespace radar::ui
