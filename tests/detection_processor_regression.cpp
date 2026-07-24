#include "BeamPatternModel.hpp"
#include "DetectionSignalProcessing.hpp"

#include <array>
#include <cstdio>

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
    constexpr float kProductionCfarThreshold = 0.26f;

    int count = 0;
    radar::app::for_each_cfar_detection(
        magnitude,
        radar::app::receive_aperture_online(pattern.active_elements),
        kProductionCfarThreshold,
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

    if (failures != 0) {
        std::fprintf(stderr,
                     "detection_processor_regression: %d failure(s)\n",
                     failures);
        return 1;
    }
    std::printf("detection_processor_regression: PASS\n");
    return 0;
}
