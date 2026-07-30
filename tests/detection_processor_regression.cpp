#include "BeamPatternModel.hpp"
#include "DetectionModel.hpp"
#include "DetectionSignalProcessing.hpp"
#include "DwellPowerAccumulator.hpp"
#include "NearRangeModel.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace {
int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

int detection_count(const radar::app::BeamPattern& pattern) {
    // A deterministic, noise-like local maximum above the production CFAR
    // threshold. This is the condition that previously created new tracks
    // after all RMAs were offline.
    constexpr std::array<float, 5> magnitude{
        0.04f, 0.08f, 0.31f, 0.07f, 0.03f};
    int count = 0;
    radar::app::for_each_cfar_detection(
        magnitude,
        radar::app::receive_aperture_online(pattern.active_elements),
        static_cast<float>(
            radar::app::detection_model::kCfarThreshold),
        [&count](int, float) { ++count; });
    return count;
}
} // namespace

int main() {
    const auto nominal = radar::app::BeamPatternModel::calculate(0u);
    check(detection_count(nominal) == 1,
          "nominal aperture reports an above-threshold local peak");

    const auto all_offline =
        radar::app::BeamPatternModel::calculate(0xFFFFu);
    check(all_offline.active_elements == 0,
          "all-offline mask produces a zero-element aperture");
    check(detection_count(all_offline) == 0,
          "all-offline aperture suppresses above-threshold noise detections");

    namespace detection = radar::app::detection_model;
    namespace near_range = radar::app::near_range;
    const double blind_range_m = detection::kRangeMinM;
    const auto disabled_near =
        near_range::observe_range(1500.0, false);
    check(!disabled_near.observable,
          "disabled sub-3 km receiver restores the hard blind range");

    const auto default_near =
        near_range::observe_range(1500.0, true);
    check(default_near.observable
              && default_near.truncated,
          "default receiver admits a pulse-eclipsed 1.5 km return");
    check(std::fabs(
              default_near.coherent_fraction
              - 1500.0 / blind_range_m) < 1.0e-12,
          "captured coherent fraction follows return delay over pulse width");
    check(std::fabs(
              default_near.apparent_range_m
              - 0.5 * (1500.0 + blind_range_m)) < 1.0e-12,
          "truncated return reports the delay-ambiguity midpoint");

    const auto at_boundary =
        near_range::observe_range(blind_range_m, false);
    const auto just_inside =
        near_range::observe_range(blind_range_m - 1.0, true);
    check(at_boundary.observable && !at_boundary.truncated
              && std::fabs(
                  at_boundary.apparent_range_m - blind_range_m)
                     < 1.0e-12,
          "normal reception begins at the exact blind-range boundary");
    check(std::fabs(
              just_inside.apparent_range_m
              - (blind_range_m - 0.5)) < 1.0e-12,
          "default apparent range is continuous at the boundary");
    check(!near_range::observe_range(0.0, true).observable
              && !near_range::observe_range(
                      detection::kRangeMaxM + 1.0,
                      true).observable,
          "default near mode remains bounded to positive instrumented range");

    const double clipped_voltage =
        near_range::observed_target_voltage(
            0.0,
            default_near,
            1.0,
            1.0);
    check(clipped_voltage
              == near_range::kTruncatedReturnMaxVoltage,
          "very strong truncated return is bounded by receiver clipping");
    const int apparent_bin =
        detection::range_bin_for(
            default_near.apparent_range_m);
    check(std::fabs(
              detection::range_m_for_bin(apparent_bin)
              - default_near.apparent_range_m)
              <= 0.5 * detection::kRangeResolutionM,
          "truncated apparent range maps to the nearest reported cell center");
    check(near_range::ambiguity_stddev_m(
              default_near.apparent_range_m) > 400.0
              && near_range::ambiguity_stddev_m(blind_range_m) == 0.0,
          "sub-3 km range uncertainty includes the truncated-delay plateau");

    radar::app::DwellPowerAccumulator dwell;
    dwell.begin(42, 45.0, 14.0, 5);
    std::array<float, 10> iq{};
    for (int pulse = 0; pulse < 10; ++pulse) {
        // Alternate carrier phase to prove that noncoherent power integration
        // preserves a moving target without requiring Doppler compensation.
        iq[4] = pulse % 2 == 0 ? 0.30f : -0.30f;
        iq[5] = 0.0f;
        dwell.accumulate(iq);
    }
    check(dwell.pulse_count() == 10,
          "one search dwell accumulates all ten PRF samples");
    std::vector<float> integrated;
    check(dwell.complete(integrated),
          "a populated dwell produces one integrated range trace");
    check(!dwell.active() && integrated.size() == 5,
          "completing a dwell resets it for the next beam");
    check(std::fabs(integrated[2] - 0.30f) < 1.0e-6f,
          "noncoherent integration retains RMS target magnitude");
    int integrated_plots = 0;
    radar::app::for_each_cfar_detection(
        integrated,
        true,
        static_cast<float>(
            radar::app::detection_model::kCfarThreshold),
        [&integrated_plots](int, float) { ++integrated_plots; });
    check(integrated_plots == 1,
          "ten pulse returns produce one dwell-level plot");

    using Clock = std::chrono::steady_clock;
    const Clock::time_point received{};
    check(radar::app::detection_model::truth_sample_fresh(
              received,
              received + radar::app::detection_model::kTruthStaleTimeout),
          "truth remains fresh at the exact timeout boundary");
    check(!radar::app::detection_model::truth_sample_fresh(
              received,
              received + radar::app::detection_model::kTruthStaleTimeout
                       + std::chrono::milliseconds(1)),
          "truth expires immediately after the timeout boundary");

    struct CachedTruth {
        Clock::time_point received_at;
    };
    std::unordered_map<int, CachedTruth> truth{
        {1, {received}},
        {2, {received + std::chrono::milliseconds(1)}}};
    const auto removed = radar::app::detection_model::prune_stale_truth(
        truth, received + radar::app::detection_model::kTruthStaleTimeout
                     + std::chrono::milliseconds(1));
    check(removed == 1 && !truth.contains(1) && truth.contains(2),
          "truth-cache pruning removes only stale target entries");

    if (failures != 0) {
        std::fprintf(stderr,
                     "detection_processor_regression: %d failure(s)\n",
                     failures);
        return 1;
    }
    std::printf("detection_processor_regression: PASS\n");
    return 0;
}
