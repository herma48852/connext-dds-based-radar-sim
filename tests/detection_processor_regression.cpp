#include "BeamPatternModel.hpp"
#include "DetectionModel.hpp"
#include "DetectionSignalProcessing.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <unordered_map>

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
