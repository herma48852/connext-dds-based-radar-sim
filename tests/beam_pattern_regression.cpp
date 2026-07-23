#include "BeamPatternModel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace {
int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}
} // namespace

int main() {
    using radar::app::BeamPatternModel;

    const auto nominal = BeamPatternModel::calculate(0u);
    check(nominal.active_elements == 1024,
          "nominal pattern has all 1024 elements");
    check(std::fabs(nominal.gain_loss_db) < 1.0e-9,
          "nominal gain loss is zero");
    check(std::fabs(nominal.boresight_error_deg) < 1.0e-9,
          "nominal boresight error is zero");
    check(nominal.beamwidth_3db_deg > 1.8 &&
          nominal.beamwidth_3db_deg < 2.2,
          "nominal 3 dB beamwidth remains approximately 2 degrees");
    check(nominal.peak_sidelobe_level_db < -10.0 &&
          nominal.peak_sidelobe_level_db > -17.0,
          "nominal peak sidelobe is in the expected ULA range");

    const auto edge = BeamPatternModel::calculate(1u << 3);
    check(edge.active_elements == 960,
          "one offline RMA removes exactly 64 elements");
    check(std::fabs(edge.active_fraction - 0.9375) < 1.0e-12,
          "one offline RMA leaves 93.75 percent active aperture");
    check(edge.gain_loss_db < -0.5 && edge.gain_loss_db > -0.7,
          "one offline RMA produces the expected gain loss");
    check(std::fabs(edge.boresight_error_deg) > 0.2,
          "asymmetric edge outage produces bounded fallback-calibration error");
    check(edge.relative_amplitude(edge.boresight_error_deg) > 0.95,
          "pattern peak follows effective boresight");

    // Total sensitivity must degrade monotonically with offline-RMA count,
    // irrespective of which geometry-dependent beam-shape metrics improve or
    // cancel for a particular symmetric mask.
    constexpr std::array<uint32_t, 6> cumulative_masks{
        0x0001u, 0x0003u, 0x000Fu, 0x00FFu, 0x0FFFu, 0xFFFFu};
    constexpr std::array<int, 6> offline_counts{1, 2, 4, 8, 12, 16};
    double previous_gain_loss = nominal.gain_loss_db;
    for (std::size_t i = 0; i < cumulative_masks.size(); ++i) {
        const auto pattern =
            BeamPatternModel::calculate(cumulative_masks[i]);
        const int expected_active = (16 - offline_counts[i]) * 64;
        const double expected_fraction = expected_active / 1024.0;
        const double expected_loss = 20.0 * std::log10(
            std::max(0.01, expected_fraction));
        check(pattern.active_elements == expected_active,
              "cumulative outage removes 64 elements per RMA");
        check(std::fabs(pattern.gain_loss_db - expected_loss) < 1.0e-9,
              "cumulative outage gain follows active-aperture fraction");
        check(pattern.gain_loss_db < previous_gain_loss,
              "gain loss worsens monotonically as RMAs go offline");
        previous_gain_loss = pattern.gain_loss_db;
    }

    const auto symmetric =
        BeamPatternModel::calculate((1u << 0) | (1u << 3));
    check(std::fabs(symmetric.boresight_error_deg) < 1.0e-9,
          "mirror-symmetric outages cancel boresight error");

    const auto center = BeamPatternModel::calculate(1u << 5);
    double max_pattern_difference = 0.0;
    for (std::size_t i = 0; i < edge.azimuth_pattern_db.size(); ++i) {
        max_pattern_difference = std::max(
            max_pattern_difference,
            std::fabs(static_cast<double>(edge.azimuth_pattern_db[i])
                    - static_cast<double>(center.azimuth_pattern_db[i])));
    }
    check(max_pattern_difference > 0.5,
          "RMA position changes the pattern, not merely active count");

    const auto offline = BeamPatternModel::calculate(0xFFFFu);
    check(offline.active_elements == 0,
          "all-offline mask removes the complete aperture");
    check(std::fabs(offline.gain_loss_db + 40.0) < 1.0e-9,
          "all-offline telemetry matches the receiver's 1 percent floor");
    check(std::all_of(offline.azimuth_pattern_db.begin(),
                      offline.azimuth_pattern_db.end(),
                      [](float db) { return std::isfinite(db) && db <= -79.0f; }),
          "all-offline pattern remains finite and dark");

    if (failures != 0) {
        std::fprintf(stderr, "beam_pattern_regression: %d failure(s)\n",
                     failures);
        return 1;
    }
    std::printf("beam_pattern_regression: PASS\n");
    return 0;
}
