#pragma once
// ============================================================================
// Visual theme: flat dark military/industrial console aesthetic.
// ============================================================================

#include <imgui.h>

namespace radar::ui::theme {

inline ImU32 col_bg()          { return IM_COL32(10, 10, 15, 255); }   // #0a0a0f
inline ImU32 col_panel()       { return IM_COL32(16, 18, 24, 255); }
inline ImU32 col_border()      { return IM_COL32(52, 58, 68, 255); }   // 1px subtle
inline ImU32 col_grid()        { return IM_COL32(30, 48, 36, 160); }
inline ImU32 col_ring()        { return IM_COL32(42, 96, 56, 200); }
inline ImU32 col_spoke()       { return IM_COL32(38, 70, 48, 140); }
inline ImU32 col_text_dim()    { return IM_COL32(120, 150, 128, 255); }
inline ImU32 col_text()        { return IM_COL32(190, 220, 195, 255); }
inline ImU32 col_sweep()       { return IM_COL32(160, 255, 120, 255); }
inline ImU32 col_phosphor()    { return IM_COL32(90, 255, 140, 255); }

// SNR-coded blip colors: green = strong, yellow = medium, red = weak
inline ImU32 col_snr(double snr_db) {
    if (snr_db >= 25.0) return IM_COL32(60, 255, 90, 255);
    if (snr_db >= 12.0) return IM_COL32(255, 220, 60, 255);
    return IM_COL32(255, 70, 60, 255);
}

inline ImU32 col_track()       { return IM_COL32(80, 220, 255, 255); } // cyan symbols
inline ImU32 col_ownship()     { return IM_COL32(230, 240, 255, 255); }

// LED-style status indicators
inline ImU32 col_led_ok()      { return IM_COL32(50, 220, 80, 255); }
inline ImU32 col_led_warn()    { return IM_COL32(255, 180, 40, 255); }
inline ImU32 col_led_fault()   { return IM_COL32(255, 60, 50, 255); }

// B-scope intensity gradient: black -> blue -> green -> yellow -> red
inline void bscope_gradient(float t, unsigned char& r, unsigned char& g, unsigned char& b) {
    t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
    float fr = 0, fg = 0, fb = 0;
    if (t < 0.25f)      { fb = t / 0.25f; }
    else if (t < 0.5f)  { fb = 1.f; fg = (t - 0.25f) / 0.25f; }
    else if (t < 0.75f) { fg = 1.f; fb = 1.f - (t - 0.5f) / 0.25f; fr = (t - 0.5f) / 0.25f; }
    else                { fr = 1.f; fg = 1.f - (t - 0.75f) / 0.25f; }
    r = (unsigned char)(fr * 255.f);
    g = (unsigned char)(fg * 255.f);
    b = (unsigned char)(fb * 255.f);
}

// Apply the flat dark style to the current ImGui context.
inline void apply_style(float content_scale) {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 6.0f;
    s.ChildRounding     = 6.0f;
    s.FrameRounding     = 4.0f;
    s.WindowBorderSize  = 1.0f;
    s.FrameBorderSize   = 1.0f;
    s.WindowPadding     = ImVec2(8, 8);
    s.ItemSpacing       = ImVec2(6, 4);
    s.AntiAliasedLines  = true;
    s.AntiAliasedFill   = true;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]      = ImVec4(0.063f, 0.071f, 0.094f, 1.0f);
    c[ImGuiCol_ChildBg]       = ImVec4(0.055f, 0.059f, 0.078f, 1.0f);
    c[ImGuiCol_Border]        = ImVec4(0.204f, 0.227f, 0.267f, 1.0f);
    c[ImGuiCol_Text]          = ImVec4(0.745f, 0.863f, 0.761f, 1.0f);
    c[ImGuiCol_TextDisabled]  = ImVec4(0.47f, 0.59f, 0.50f, 1.0f);
    c[ImGuiCol_FrameBg]       = ImVec4(0.10f, 0.12f, 0.15f, 1.0f);
    c[ImGuiCol_Button]        = ImVec4(0.12f, 0.18f, 0.14f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.18f, 0.30f, 0.20f, 1.0f);
    c[ImGuiCol_ButtonActive]  = ImVec4(0.24f, 0.42f, 0.27f, 1.0f);
    c[ImGuiCol_Header]        = ImVec4(0.14f, 0.22f, 0.16f, 1.0f);
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.10f, 0.16f, 0.12f, 1.0f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(0.07f, 0.09f, 0.08f, 1.0f);

    s.ScaleAllSizes(content_scale); // Retina / HiDPI crispness
}

} // namespace radar::ui::theme
