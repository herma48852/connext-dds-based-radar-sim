#pragma once

#include <chrono>
#include <cstddef>

namespace radar::app::detection_model {

inline constexpr int    kRangeBins      = 512;
inline constexpr double kRangeMaxM      = 100000.0;
inline constexpr double kBeamwidthDeg   = 2.0;
inline constexpr double kNoiseSigma     = 0.05;
inline constexpr double kCfarThreshold  = 0.26;
inline constexpr double kSignalScale    = 3.0e8;

// TargetTruth is published at 50 Hz. Allow 25 consecutive updates to be lost
// before removing a target, while still reacting promptly when target_gen
// exits or restarts with a smaller fleet.
inline constexpr auto kTruthStaleTimeout = std::chrono::milliseconds(500);

template <typename TimePoint>
constexpr bool truth_sample_fresh(TimePoint received_at, TimePoint now) {
    return now - received_at <= kTruthStaleTimeout;
}

template <typename Map, typename TimePoint>
std::size_t prune_stale_truth(Map& truth, TimePoint now) {
    std::size_t removed = 0;
    for (auto it = truth.begin(); it != truth.end();) {
        if (!truth_sample_fresh(it->second.received_at, now)) {
            it = truth.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

} // namespace radar::app::detection_model
