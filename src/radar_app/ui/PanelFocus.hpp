#pragma once

#include <algorithm>

#include <imgui.h>

namespace radar::ui {

enum class PanelId {
    None,
    Ppi,
    AScope,
    BScope,
    TrackList,
    BeamTimeline,
    SystemHealth,
    ShipPosition,
    ArrayFace,
    Scenarios
};

class PanelFocusState {
public:
    bool active() const noexcept { return focused_ != PanelId::None; }
    bool is_focused(PanelId panel) const noexcept {
        return focused_ == panel;
    }
    PanelId focused() const noexcept { return focused_; }

    void toggle(PanelId panel) noexcept {
        focused_ = is_focused(panel) ? PanelId::None : panel;
        focus_request_ = focused_;
    }

    void clear() noexcept {
        focused_ = PanelId::None;
        focus_request_ = PanelId::None;
    }

    bool consume_focus_request(PanelId panel) noexcept {
        if (focus_request_ != panel)
            return false;
        focus_request_ = PanelId::None;
        return true;
    }

    static constexpr float presenter_scale() noexcept {
        return 17.0f / 13.0f;
    }

private:
    PanelId focused_{PanelId::None};
    PanelId focus_request_{PanelId::None};
};

// Draw a compact expand/contract affordance in the panel title bar. The
// explicit full-window clip makes the control interactive above ImGui's
// normal content clip without depending on imgui_internal.h.
inline void render_panel_focus_control(
        PanelId panel, PanelFocusState* state) {
    if (!state)
        return;

    if (state->consume_focus_request(panel))
        ImGui::SetWindowFocus();

    const ImVec2 window_pos = ImGui::GetWindowPos();
    const ImVec2 window_size = ImGui::GetWindowSize();
    const float title_height = ImGui::GetFrameHeight();
    const float side = std::max(16.0f, title_height - 4.0f);
    const ImVec2 button_pos{
        window_pos.x + window_size.x - side - 4.0f,
        window_pos.y + 2.0f};

    const ImVec2 saved_cursor = ImGui::GetCursorScreenPos();
    ImGui::PushClipRect(
        window_pos,
        ImVec2(window_pos.x + window_size.x,
               window_pos.y + window_size.y),
        false);
    ImGui::PushID(static_cast<int>(panel));
    ImGui::SetCursorScreenPos(button_pos);
    const bool clicked =
        ImGui::InvisibleButton("##panel_focus", ImVec2(side, side));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 fill = ImGui::GetColorU32(
        held ? ImGuiCol_ButtonActive
             : hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button);
    const ImU32 border = ImGui::GetColorU32(ImGuiCol_Border);
    const ImU32 icon = ImGui::GetColorU32(ImGuiCol_Text);
    const ImVec2 button_max{button_pos.x + side, button_pos.y + side};
    draw->AddRectFilled(button_pos, button_max, fill, 3.0f);
    draw->AddRect(button_pos, button_max, border, 3.0f);

    const float inset = 4.0f;
    const float arm = std::max(3.0f, side * 0.22f);
    const float left = button_pos.x + inset;
    const float top = button_pos.y + inset;
    const float right = button_max.x - inset;
    const float bottom = button_max.y - inset;
    if (state->is_focused(panel)) {
        // Four corners point inward when the next action is "contract".
        draw->AddLine(
            ImVec2(left, top + arm), ImVec2(left + arm, top + arm),
            icon, 1.5f);
        draw->AddLine(
            ImVec2(left + arm, top), ImVec2(left + arm, top + arm),
            icon, 1.5f);
        draw->AddLine(
            ImVec2(right - arm, top), ImVec2(right - arm, top + arm),
            icon, 1.5f);
        draw->AddLine(
            ImVec2(right - arm, top + arm), ImVec2(right, top + arm),
            icon, 1.5f);
        draw->AddLine(
            ImVec2(left, bottom - arm), ImVec2(left + arm, bottom - arm),
            icon, 1.5f);
        draw->AddLine(
            ImVec2(left + arm, bottom - arm), ImVec2(left + arm, bottom),
            icon, 1.5f);
        draw->AddLine(
            ImVec2(right - arm, bottom - arm), ImVec2(right, bottom - arm),
            icon, 1.5f);
        draw->AddLine(
            ImVec2(right - arm, bottom - arm), ImVec2(right - arm, bottom),
            icon, 1.5f);
    } else {
        // Four outward corners when the next action is "expand".
        draw->AddLine(
            ImVec2(left, top), ImVec2(left + arm, top), icon, 1.5f);
        draw->AddLine(
            ImVec2(left, top), ImVec2(left, top + arm), icon, 1.5f);
        draw->AddLine(
            ImVec2(right - arm, top), ImVec2(right, top), icon, 1.5f);
        draw->AddLine(
            ImVec2(right, top), ImVec2(right, top + arm), icon, 1.5f);
        draw->AddLine(
            ImVec2(left, bottom - arm), ImVec2(left, bottom), icon, 1.5f);
        draw->AddLine(
            ImVec2(left, bottom), ImVec2(left + arm, bottom), icon, 1.5f);
        draw->AddLine(
            ImVec2(right, bottom - arm), ImVec2(right, bottom), icon, 1.5f);
        draw->AddLine(
            ImVec2(right - arm, bottom), ImVec2(right, bottom), icon, 1.5f);
    }

    if (hovered) {
        ImGui::SetTooltip(
            "%s panel",
            state->is_focused(panel) ? "Contract" : "Expand");
    }

    ImGui::PopID();
    ImGui::PopClipRect();
    ImGui::SetCursorScreenPos(saved_cursor);

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool title_double_clicked =
        !hovered
        && ImGui::IsWindowHovered(
            ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)
        && mouse.y >= window_pos.y
        && mouse.y <= window_pos.y + title_height
        && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    if (clicked || title_double_clicked)
        state->toggle(panel);
}

} // namespace radar::ui
