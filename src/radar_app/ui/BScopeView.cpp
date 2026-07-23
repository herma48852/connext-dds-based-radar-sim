#include "BScopeView.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>

#if defined(__APPLE__)
#include "MetalContext.hpp"
#endif

#include "Theme.hpp"
#include "../components/BeamPatternModel.hpp"

namespace radar::ui {

BScopeView::~BScopeView() {
    // GPU context may already be gone; release_*() handles the normal path.
}

#if defined(__APPLE__)

void BScopeView::release_texture(MetalContext& ctx) {
    if (tex_ != nullptr) {
        ctx.destroy_texture(tex_);
        tex_ = nullptr;
    }
    ctx_ = nullptr;
}

void BScopeView::init_texture(MetalContext& ctx) {
    ctx_ = &ctx;
    if (tex_ == nullptr) {
        tex_ = ctx.create_texture(kAzBins, kRangeBins);
        rgba_.assign(size_t(kAzBins) * kRangeBins * 4, 0);
    }
    if (!lut_built_) {
        for (int i = 0; i < 256; ++i) {
            unsigned char r, g, b;
            theme::bscope_gradient(i / 255.0f, r, g, b);
            lut_[i][0] = r; lut_[i][1] = g; lut_[i][2] = b; lut_[i][3] = 255;
        }
        lut_built_ = true;
    }
}

#else // !__APPLE__

void BScopeView::release_gl() {
    if (tex_ != 0) {
        glDeleteTextures(1, &tex_);
        tex_ = 0;
    }
}

void BScopeView::init_gl() {
    if (tex_ == 0) {
        glGenTextures(1, &tex_);
        glBindTexture(GL_TEXTURE_2D, tex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kAzBins, kRangeBins, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
        rgba_.assign(size_t(kAzBins) * kRangeBins * 4, 0);
    }
    if (!lut_built_) {
        for (int i = 0; i < 256; ++i) {
            unsigned char r, g, b;
            theme::bscope_gradient(i / 255.0f, r, g, b);
            lut_[i][0] = r; lut_[i][1] = g; lut_[i][2] = b; lut_[i][3] = 255;
        }
        lut_built_ = true;
    }
}

#endif // __APPLE__

void BScopeView::splat(const app::BlipView& b) {
    // Reject NaN/Inf blips outright: casting them to int is UB.
    if (!std::isfinite(b.azimuth_deg) || !std::isfinite(b.range_m) ||
        !std::isfinite(b.snr_db))
        return;
    // Mod in long arithmetic (no narrowing cast of a huge double), then
    // clamp in double before converting — indices stay in range for any
    // finite input, however extreme.
    const long az_l = std::lround(b.azimuth_deg);
    const int az = static_cast<int>((az_l % 360 + 360) % 360);
    const int rb = static_cast<int>(std::clamp(
        b.range_m / kRangeMaxM * kRangeBins, 0.0,
        static_cast<double>(kRangeBins - 1)));
    const float amp = std::clamp((float)(b.snr_db / 30.0), 0.15f, 1.0f);
    // 3x3 gaussian splat
    for (int da = -1; da <= 1; ++da) {
        for (int dr = -1; dr <= 1; ++dr) {
            const int a = (az + da + 360) % 360;
            const int r = rb + dr;
            if (r < 0 || r >= kRangeBins) continue;
            const float w = (da == 0 && dr == 0) ? 1.0f : 0.4f;
            float& cell = heat_[size_t(r) * kAzBins + a];
            cell = std::min(1.0f, cell + amp * w);
        }
    }
}

void BScopeView::render(const char* title, ImVec2 pos, ImVec2 size,
                        const std::vector<app::TrackView>& tracks,
                        const app::ShipView& ship,
                        bool sector_mode, double sector_center, double sector_width,
                        double live_beam_az_deg,
                        bool show_beam_formation,
                        const app::BeamPatternView& beam_pattern,
                        float dt) {
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::Begin(title, nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoResize);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = ImGui::GetContentRegionAvail().y - 4.0f;

    dl->AddRectFilled(wp, ImVec2(wp.x + w, wp.y + h), theme::col_bg());
    dl->AddRect(wp, ImVec2(wp.x + w, wp.y + h), theme::col_border());

    if (tex_ != 0) {
        // decay heat, then convert through the LUT and upload
        const float decay = std::pow(0.5f, dt / 4.0f);   // 4 s half-life
        for (size_t i = 0; i < heat_.size(); ++i) {
            heat_[i] *= decay;
            const unsigned char v = (unsigned char)std::clamp(heat_[i] * 255.0f, 0.0f, 255.0f);
            const auto* c = lut_[v];
            // row 0 of the texture = far range (top of display)
            rgba_[i*4+0] = c[0]; rgba_[i*4+1] = c[1];
            rgba_[i*4+2] = c[2]; rgba_[i*4+3] = c[3];
        }
        // flip vertically: heat_ row 0 = near range -> bottom of texture
        static thread_local std::vector<unsigned char> flipped;
        flipped.resize(rgba_.size());
        const size_t row_bytes = size_t(kAzBins) * 4;
        for (int r = 0; r < kRangeBins; ++r)
            std::copy_n(rgba_.data() + size_t(r) * row_bytes, row_bytes,
                        flipped.data() + size_t(kRangeBins - 1 - r) * row_bytes);

        // Upload decimation (--gl-throttle): 360x256 RGBA per frame is
        // ~22 MB/s. Harmless for native Metal, but the knob stays — at 4x
        // decimation (15 Hz) the phosphor decay looks identical.
        if (upload_frame_++ % upload_decimation_ == 0) {
#if defined(__APPLE__)
            ctx_->upload_texture(tex_, flipped.data(), kAzBins, kRangeBins);
#else
            glBindTexture(GL_TEXTURE_2D, tex_);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kAzBins, kRangeBins,
                            GL_RGBA, GL_UNSIGNED_BYTE, flipped.data());
            glBindTexture(GL_TEXTURE_2D, 0);
#endif
        }

#if defined(__APPLE__)
        ImGui::Image((ImTextureID)tex_, ImVec2(w, h)); // bridged MTLTexture
#else
        ImGui::Image((ImTextureID)(intptr_t)tex_, ImVec2(w, h));
#endif
    }

    // --- overlay mapping helpers (screen space) ---
    auto x_of_az = [&](double az) { return wp.x + (float)(az / 360.0) * w; };
    auto y_of_range = [&](double r) {
        return wp.y + h - (float)(r / kRangeMaxM) * h;
    };

    // --- automatic compact outage overlay ---------------------------------
    // Preserve the original moving response curtain as the at-a-glance
    // failure indication when the operator has not selected the detailed
    // BEAM FORMATION comparison view.
    const bool show_marching_outage_curtain =
        !show_beam_formation &&
        (beam_pattern.rma_mask & 0xFFFFu) != 0u &&
        !beam_pattern.azimuth_pattern_db.empty() &&
        beam_pattern.pattern_step_deg > 0.0;
    if (show_marching_outage_curtain) {
        auto wrap360 = [](double a) {
            while (a >= 360.0) a -= 360.0;
            while (a < 0.0) a += 360.0;
            return a;
        };
        auto vertical_line = [&](double az, ImU32 color, float thickness) {
            const float x = x_of_az(wrap360(az));
            dl->AddLine(ImVec2(x, wp.y), ImVec2(x, wp.y + h),
                        color, thickness);
        };
        auto dashed_vertical = [&](double az, ImU32 color, float thickness,
                                   float dash, float gap) {
            const float x = x_of_az(wrap360(az));
            for (float y = wp.y; y < wp.y + h; y += dash + gap) {
                dl->AddLine(ImVec2(x, y),
                            ImVec2(x, std::min(y + dash, wp.y + h)),
                            color, thickness);
            }
        };

        const float sample_px = std::max(
            1.0f, static_cast<float>(
                beam_pattern.pattern_step_deg / 360.0 * w));
        for (std::size_t i = 0;
             i < beam_pattern.azimuth_pattern_db.size(); ++i) {
            const double db = beam_pattern.azimuth_pattern_db[i];
            if (!std::isfinite(db) || db < -24.0)
                continue;
            const double offset = beam_pattern.pattern_start_offset_deg
                                + i * beam_pattern.pattern_step_deg;
            const float x = x_of_az(wrap360(live_beam_az_deg + offset));
            const float x0 = std::max(wp.x, x - sample_px * 0.55f);
            const float x1 = std::min(wp.x + w, x + sample_px * 0.55f);
            if (x1 <= x0)
                continue;
            if (db >= -3.0) {
                dl->AddRectFilled(
                    ImVec2(x0, wp.y), ImVec2(x1, wp.y + h),
                    IM_COL32(255, 190, 40, 28));
            } else {
                const int alpha = std::clamp(
                    static_cast<int>(4.0 + (db + 24.0) / 21.0 * 16.0),
                    4, 20);
                dl->AddRectFilled(
                    ImVec2(x0, wp.y), ImVec2(x1, wp.y + h),
                    IM_COL32(255, 70, 55, alpha));
            }
        }

        const int offline_count =
            std::popcount(beam_pattern.rma_mask & 0xFFFFu);
        const bool fully_offline = offline_count == 16;
        const double effective_az =
            live_beam_az_deg + beam_pattern.boresight_error_deg;
        dashed_vertical(live_beam_az_deg, IM_COL32(100, 255, 140, 210),
                        1.2f, 6.0f, 3.0f);
        if (!fully_offline) {
            vertical_line(effective_az, IM_COL32(255, 195, 55, 230), 1.8f);
            if (beam_pattern.peak_sidelobe_level_db > -30.0) {
                dashed_vertical(
                    live_beam_az_deg
                        + beam_pattern.left_sidelobe_offset_deg,
                    IM_COL32(255, 80, 65, 180), 1.2f, 3.0f, 4.0f);
                dashed_vertical(
                    live_beam_az_deg
                        + beam_pattern.right_sidelobe_offset_deg,
                    IM_COL32(255, 80, 65, 180), 1.2f, 3.0f, 4.0f);
            }
        }

        char status[176];
        if (fully_offline) {
            std::snprintf(status, sizeof status,
                          "ARRAY OFFLINE  RMA 16/16  LOSS %.0f dB",
                          beam_pattern.gain_loss_db);
        } else {
            std::snprintf(
                status, sizeof status,
                "BEAM DEGRADED  RMA %d/16  LOSS %.1f dB  BW %.1f  "
                "PSL %.0f  ERR %+.1f",
                offline_count,
                beam_pattern.gain_loss_db,
                beam_pattern.beamwidth_3db_deg,
                beam_pattern.peak_sidelobe_level_db,
                beam_pattern.boresight_error_deg);
        }
        const ImVec2 status_size = ImGui::CalcTextSize(status);
        const ImVec2 status_pos(
            std::max(wp.x + 4.0f, wp.x + w - status_size.x - 7.0f),
            wp.y + 4.0f);
        dl->AddRectFilled(
            ImVec2(status_pos.x - 3.0f, status_pos.y - 2.0f),
            ImVec2(status_pos.x + status_size.x + 3.0f,
                   status_pos.y + status_size.y + 2.0f),
            IM_COL32(10, 10, 15, 215), 2.0f);
        dl->AddText(status_pos, IM_COL32(255, 205, 80, 255), status);
    }

    if (!show_beam_formation) {
        beam_comparison_mix_ = 0.0f;
        if ((beam_pattern.rma_mask & 0xFFFFu) == 0u)
            have_last_degraded_pattern_ = false;
    }

    // --- operator-selected beam-formation comparison -----------------------
    // Unlike the automatic curtain, this view is also available while the
    // array is nominal so the operator has a before/after reference.
    if (show_beam_formation &&
        !beam_pattern.azimuth_pattern_db.empty() &&
        beam_pattern.pattern_step_deg > 0.0) {
        auto wrap360 = [](double a) {
            while (a >= 360.0) a -= 360.0;
            while (a < 0.0) a += 360.0;
            return a;
        };

        const int offline_count =
            std::popcount(beam_pattern.rma_mask & 0xFFFFu);
        const bool fully_offline = offline_count == 16;
        if (offline_count > 0) {
            last_degraded_pattern_ = beam_pattern;
            have_last_degraded_pattern_ = true;
        }
        const float comparison_target = offline_count > 0 ? 1.0f : 0.0f;
        const float comparison_step =
            std::clamp(dt, 0.0f, 0.1f) / 0.65f;
        if (beam_comparison_mix_ < comparison_target) {
            beam_comparison_mix_ = std::min(
                comparison_target, beam_comparison_mix_ + comparison_step);
        } else if (beam_comparison_mix_ > comparison_target) {
            beam_comparison_mix_ = std::max(
                comparison_target, beam_comparison_mix_ - comparison_step);
        }
        const bool returning_to_nominal =
            offline_count == 0 && beam_comparison_mix_ > 0.0f
            && have_last_degraded_pattern_;
        if (beam_comparison_mix_ <= 0.0f && offline_count == 0)
            have_last_degraded_pattern_ = false;

        char status_top[192];
        char status_detail[192];
        if (fully_offline) {
            std::snprintf(
                status_top, sizeof status_top,
                "BEAM FORMATION  |  ARRAY OFFLINE  |  SCAN BEAM %.1f deg",
                wrap360(live_beam_az_deg));
            std::snprintf(status_detail, sizeof status_detail,
                          "RMA 16/16 OFFLINE  |  GAIN LOSS %.0f dB",
                          beam_pattern.gain_loss_db);
        } else if (returning_to_nominal) {
            std::snprintf(
                status_top, sizeof status_top,
                "BEAM FORMATION  |  ARRAY RESTORED  |  "
                "NOMINAL RECENTERING");
            std::snprintf(
                status_detail, sizeof status_detail,
                "RMA 0/16 OFFLINE  |  DEGRADED COMPARISON RETIRING");
        } else if (offline_count == 0) {
            std::snprintf(
                status_top, sizeof status_top,
                "BEAM FORMATION  |  LIVE NOMINAL  |  SCAN BEAM %.1f deg",
                wrap360(live_beam_az_deg));
            std::snprintf(
                status_detail, sizeof status_detail,
                "RMA 0/16 OFFLINE  |  SELECT AN RMA TO COMPARE WITH NOMINAL");
        } else {
            std::snprintf(
                status_top, sizeof status_top,
                "BEAM FORMATION  |  LIVE DEGRADED RMA %d/16  |  "
                "SCAN BEAM %.1f deg",
                offline_count,
                wrap360(live_beam_az_deg));
            std::snprintf(
                status_detail, sizeof status_detail,
                "GAIN LOSS %.1f dB  |  WIDTH %.1f deg  |  POINTING %+.1f deg"
                "  |  PEAK SIDELOBE %.0f dB",
                beam_pattern.gain_loss_db,
                beam_pattern.beamwidth_3db_deg,
                beam_pattern.boresight_error_deg,
                beam_pattern.peak_sidelobe_level_db);
        }

        // Fall back to compact text when the B-scope is narrow.
        ImVec2 top_size = ImGui::CalcTextSize(status_top);
        ImVec2 detail_size = ImGui::CalcTextSize(status_detail);
        if (std::max(top_size.x, detail_size.x) > w - 14.0f) {
            if (fully_offline) {
                std::snprintf(status_top, sizeof status_top,
                              "ARRAY OFFLINE | SCAN BEAM %.1f deg ->",
                              wrap360(live_beam_az_deg));
                std::snprintf(status_detail, sizeof status_detail,
                              "RMA 16/16 | LOSS %.0f dB",
                              beam_pattern.gain_loss_db);
            } else if (returning_to_nominal) {
                std::snprintf(status_top, sizeof status_top,
                              "ARRAY RESTORED | NOMINAL RECENTERING");
                std::snprintf(status_detail, sizeof status_detail,
                              "RMA 0/16 | DEGRADED VIEW RETIRING");
            } else if (offline_count == 0) {
                std::snprintf(status_top, sizeof status_top,
                              "BEAM FORMATION | NOMINAL | BEAM %.1f deg",
                              wrap360(live_beam_az_deg));
                std::snprintf(status_detail, sizeof status_detail,
                              "RMA 0/16 | SELECT AN RMA TO COMPARE");
            } else {
                std::snprintf(status_top, sizeof status_top,
                              "RMA %d/16 | SCAN BEAM %.1f deg ->",
                              offline_count, wrap360(live_beam_az_deg));
                std::snprintf(
                    status_detail, sizeof status_detail,
                    "LOSS %.1f | WIDTH %.1f | POINT %.1f | SIDE %.0f dB",
                    beam_pattern.gain_loss_db,
                    beam_pattern.beamwidth_3db_deg,
                    beam_pattern.boresight_error_deg,
                    beam_pattern.peak_sidelobe_level_db);
            }
            top_size = ImGui::CalcTextSize(status_top);
            detail_size = ImGui::CalcTextSize(status_detail);
        }

        const float text_gap = 2.0f;
        const float status_w = std::max(top_size.x, detail_size.x);
        const float status_h = top_size.y + text_gap + detail_size.y;
        const ImVec2 text_pos(
            std::max(wp.x + 4.0f, wp.x + w - status_w - 7.0f),
            wp.y + 4.0f);
        dl->AddRectFilled(
            ImVec2(text_pos.x - 3.0f, text_pos.y - 2.0f),
            ImVec2(text_pos.x + status_w + 3.0f,
                   text_pos.y + status_h + 2.0f),
            IM_COL32(10, 10, 15, 215), 2.0f);
        dl->AddText(text_pos, IM_COL32(255, 205, 80, 255), status_top);
        dl->AddText(
            ImVec2(text_pos.x, text_pos.y + top_size.y + text_gap),
            IM_COL32(220, 195, 125, 255), status_detail);

        // Stateful top-down comparison. Nominal starts centered. An outage
        // moves it to the far left while the degraded live pattern fades in
        // at center. Restoring the RMA reverses the same transition while
        // the cached degraded sample remains available for visual continuity.
        // Give beam formation the B-scope instead of treating it as a small
        // diagnostic inset.  Using almost the full available scope makes
        // each split comparison plot a little over twice its former radius
        // at the standard console layout.
        const float inset_w = std::max(1.0f, w - 16.0f);
        const ImVec2 inset_min(
            wp.x + 8.0f,
            wp.y + std::max(38.0f, status_h + 14.0f));
        const float inset_h = std::max(
            1.0f, wp.y + h - inset_min.y - 8.0f);
        const ImVec2 inset_max(inset_min.x + inset_w,
                               inset_min.y + inset_h);
        dl->AddRectFilled(inset_min, inset_max, IM_COL32(7, 9, 13, 232),
                          3.0f);
        dl->AddRect(inset_min, inset_max, IM_COL32(120, 105, 65, 210),
                    3.0f);
        dl->AddText(
            ImVec2(inset_min.x + 7.0f, inset_min.y + 5.0f),
            IM_COL32(255, 215, 105, 255),
            "TOP-DOWN BEAM SHAPE - ANGULAR ZOOM: -10 TO +10 deg");

        const float legend_y =
            inset_min.y + 8.0f + ImGui::GetFontSize();
        constexpr double kPi = 3.14159265358979323846;
        constexpr double kDisplayLimitDeg = 10.0;
        constexpr double kDisplayAngleScale = 45.0 / kDisplayLimitDeg;
        const float plot_top =
            legend_y + ImGui::GetFontSize() + 12.0f;
        const float origin_y = inset_max.y - 27.0f;
        const float single_radius = std::max(
            25.0f,
            std::min(origin_y - plot_top, inset_w * 0.43f));
        const float split_radius = std::max(
            25.0f,
            std::min(origin_y - plot_top, inset_w * 0.22f));
        const float comparison_mix =
            std::clamp(beam_comparison_mix_, 0.0f, 1.0f);
        const float nominal_radius =
            single_radius
            + (split_radius - single_radius) * comparison_mix;
        const float nominal_center_x =
            inset_min.x + inset_w
                * (0.50f + (0.17f - 0.50f) * comparison_mix);
        const ImVec2 nominal_origin(nominal_center_x, origin_y);
        const ImVec2 degraded_origin(inset_min.x + inset_w * 0.50f,
                                     origin_y);

        const app::BeamPatternView* degraded_pattern = nullptr;
        if (offline_count > 0) {
            degraded_pattern = &beam_pattern;
        } else if (have_last_degraded_pattern_) {
            degraded_pattern = &last_degraded_pattern_;
        }
        const int compared_offline_count = degraded_pattern
            ? std::popcount(degraded_pattern->rma_mask & 0xFFFFu) : 0;

        auto fade_color = [](ImU32 color, float alpha) {
            const auto base_alpha = (color >> 24) & 0xFFu;
            const auto faded_alpha = static_cast<ImU32>(
                std::clamp(alpha, 0.0f, 1.0f) * base_alpha);
            return (color & 0x00FFFFFFu) | (faded_alpha << 24);
        };
        auto edge_at = [&](ImVec2 origin, double offset_deg, float radius) {
            const double angle = offset_deg * kPi / 180.0;
            return ImVec2(
                origin.x + std::sin(angle) * radius,
                origin.y - std::cos(angle) * radius);
        };
        auto point_at = [&](ImVec2 origin, float outer_radius,
                            double offset_deg, double db,
                            double angular_spread) {
            const double clipped = std::clamp(db, -30.0, 0.0);
            const float radius = static_cast<float>(
                (clipped + 30.0) / 30.0) * outer_radius;
            const double display_angle =
                offset_deg * angular_spread * kDisplayAngleScale;
            const double angle = display_angle * kPi / 180.0;
            return ImVec2(
                origin.x + std::sin(angle) * radius,
                origin.y - std::cos(angle) * radius);
        };

        auto draw_grid = [&](ImVec2 origin, float outer_radius,
                             float alpha) {
            for (double db : {-20.0, -10.0, 0.0}) {
                const float radius =
                    static_cast<float>((db + 30.0) / 30.0) * outer_radius;
                ImVec2 previous = edge_at(origin, -45.0, radius);
                for (int step = 1; step <= 30; ++step) {
                    const double angle = -45.0 + step * 3.0;
                    const ImVec2 point = edge_at(origin, angle, radius);
                    dl->AddLine(
                        previous, point,
                        fade_color(IM_COL32(85, 85, 100, 105), alpha));
                    previous = point;
                }
                if (alpha > 0.35f) {
                    char db_label[12];
                    std::snprintf(db_label, sizeof db_label, "%.0f", db);
                    dl->AddText(
                        ImVec2(origin.x + 3.0f,
                               origin.y - radius - ImGui::GetFontSize()),
                        fade_color(theme::col_text_dim(), alpha), db_label);
                }
            }
            for (double angle : {-45.0, 0.0, 45.0}) {
                const ImVec2 edge = edge_at(origin, angle, outer_radius);
                dl->AddLine(
                    origin, edge,
                    fade_color(
                        angle == 0.0
                            ? IM_COL32(105, 255, 145, 150)
                            : IM_COL32(85, 85, 100, 95),
                        alpha),
                    angle == 0.0 ? 1.2f : 1.0f);
            }
            if (alpha > 0.35f) {
                const char* left_label = "-10";
                const char* center_label = "0 deg";
                const char* right_label = "+10";
                const ImVec2 left_edge =
                    edge_at(origin, -45.0, outer_radius);
                const ImVec2 right_edge =
                    edge_at(origin, 45.0, outer_radius);
                const ImVec2 left_size = ImGui::CalcTextSize(left_label);
                const ImVec2 center_size = ImGui::CalcTextSize(center_label);
                dl->AddText(
                    ImVec2(left_edge.x - left_size.x,
                           left_edge.y - ImGui::GetFontSize()),
                    fade_color(theme::col_text_dim(), alpha), left_label);
                dl->AddText(
                    ImVec2(origin.x - center_size.x * 0.5f, origin.y + 3.0f),
                    fade_color(IM_COL32(105, 255, 145, 255), alpha),
                    center_label);
                dl->AddText(
                    ImVec2(right_edge.x,
                           right_edge.y - ImGui::GetFontSize()),
                    fade_color(theme::col_text_dim(), alpha), right_label);
            }
        };

        static const app::BeamPattern nominal_pattern =
            app::BeamPatternModel::calculate(0u);

        auto draw_pattern = [&](ImVec2 origin, float outer_radius,
                                int sample_count, double start_deg,
                                double step_deg, auto db_at,
                                double gain_loss_db, bool degraded,
                                double angular_spread, float alpha) {
            bool have_previous = false;
            ImVec2 previous;
            double previous_db = -30.0;
            for (int i = 0; i < sample_count; ++i) {
                const double db = db_at(i);
                if (!std::isfinite(db)) {
                    have_previous = false;
                    continue;
                }
                const double offset = start_deg + i * step_deg;
                if (std::fabs(offset * angular_spread) >
                    kDisplayLimitDeg) {
                    have_previous = false;
                    continue;
                }
                const ImVec2 point =
                    point_at(origin, outer_radius, offset,
                             db + gain_loss_db, angular_spread);
                if (have_previous) {
                    const bool main_lobe =
                        std::max(previous_db, db) >= -3.0;
                    const ImU32 outline = degraded
                        ? (main_lobe
                            ? IM_COL32(255, 195, 55, 255)
                            : IM_COL32(255, 85, 70, 235))
                        : IM_COL32(90, 175, 255, 245);
                    const ImU32 fill = degraded
                        ? (main_lobe
                            ? IM_COL32(255, 190, 45, 34)
                            : IM_COL32(255, 70, 55, 24))
                        : IM_COL32(70, 155, 255, 22);
                    dl->AddTriangleFilled(
                        origin, previous, point, fade_color(fill, alpha));
                    dl->AddLine(previous, point,
                                fade_color(outline, alpha),
                                main_lobe ? 2.4f : 1.6f);
                }
                previous = point;
                previous_db = db;
                have_previous = true;
            }
        };

        draw_grid(nominal_origin, nominal_radius, 1.0f);
        draw_pattern(
            nominal_origin, nominal_radius,
            app::kBeamPatternSampleCount,
            app::kBeamPatternStartDeg, app::kBeamPatternStepDeg,
            [&](int i) {
                return static_cast<double>(
                    nominal_pattern.azimuth_pattern_db[i]);
            },
            0.0, false, 1.0, 1.0f);

        const char* nominal_label =
            comparison_mix > 0.05f
                ? "NOMINAL REFERENCE" : "NOMINAL - ALL RMAs ONLINE";
        const ImVec2 nominal_label_size = ImGui::CalcTextSize(nominal_label);
        dl->AddText(
            ImVec2(nominal_origin.x - nominal_label_size.x * 0.5f, legend_y),
            IM_COL32(90, 175, 255, 255), nominal_label);

        if (degraded_pattern && comparison_mix > 0.0f) {
            // A single 64-element RMA is only 6.25% of the aperture, so its
            // true beamwidth change is difficult to see at console scale.
            // Accentuate only the degraded plot's angular spread for the
            // training comparison. Published metrics and radial gain remain
            // physical; the label makes the visual-only multiplier explicit.
            const double display_spread = std::min(
                2.75, 1.0 + 0.35 * compared_offline_count);
            draw_grid(degraded_origin, split_radius, comparison_mix);
            draw_pattern(
                degraded_origin, split_radius,
                static_cast<int>(
                    degraded_pattern->azimuth_pattern_db.size()),
                degraded_pattern->pattern_start_offset_deg,
                degraded_pattern->pattern_step_deg,
                [&](int i) {
                    return static_cast<double>(
                        degraded_pattern->azimuth_pattern_db[
                            static_cast<std::size_t>(i)]);
                },
                degraded_pattern->gain_loss_db, true,
                display_spread, comparison_mix);

            char degraded_label[96];
            std::snprintf(degraded_label, sizeof degraded_label,
                          "DEGRADED RMA %d/16 | DISPLAY SPREAD %.1fx",
                          compared_offline_count, display_spread);
            const ImVec2 degraded_label_size =
                ImGui::CalcTextSize(degraded_label);
            dl->AddText(
                ImVec2(
                    degraded_origin.x - degraded_label_size.x * 0.5f,
                    legend_y),
                fade_color(IM_COL32(255, 195, 55, 255),
                           comparison_mix),
                degraded_label);
        }
    }

    // dashed sector boundary lines
    if (sector_mode) {
        for (double edge : {sector_center - sector_width / 2, sector_center + sector_width / 2}) {
            double a = std::fmod(edge + 360.0, 360.0);
            const float x = x_of_az(a);
            for (float y = wp.y; y < wp.y + h; y += 8.0f)
                dl->AddLine(ImVec2(x, y), ImVec2(x, std::min(y + 4.0f, wp.y + h)),
                            IM_COL32(255, 220, 80, 160), 1.0f);
        }
    }

    // track markers with IDs
    for (const auto& t : tracks) {
        const double range = std::hypot(t.x_m, t.y_m);
        if (range > kRangeMaxM) continue;
        const double az_world = std::atan2(t.x_m, t.y_m) * 180.0 / 3.14159265358979323846;
        double az_ship = std::fmod(az_world - ship.heading_deg + 360.0, 360.0);
        const ImVec2 p(x_of_az(az_ship), y_of_range(range));
        dl->AddRect(ImVec2(p.x - 4, p.y - 4), ImVec2(p.x + 4, p.y + 4),
                    theme::col_track(), 0.0f, 0, 1.5f);
        char lbl[16];
        std::snprintf(lbl, sizeof lbl, "%lld", (long long)t.track_id);
        dl->AddText(ImVec2(p.x + 6, p.y - 6), theme::col_text(), lbl);
    }

    // axis labels
    char lbl[32];
    for (int deg = 0; deg < 360; deg += 60) {
        std::snprintf(lbl, sizeof lbl, "%03d", deg);
        dl->AddText(ImVec2(x_of_az(deg) - 8, wp.y + h - 14), theme::col_text_dim(), lbl);
    }
    std::snprintf(lbl, sizeof lbl, "%.0fkm", kRangeMaxM / 1000.0);
    dl->AddText(ImVec2(wp.x + 4, wp.y + 4), theme::col_text_dim(), lbl);

    ImGui::Dummy(ImVec2(w, h));
    ImGui::End();
}

} // namespace radar::ui
