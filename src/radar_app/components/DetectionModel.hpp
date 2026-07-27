#pragma once

#include <chrono>
#include <cmath>
#include <cstddef>

#include "RadarRfModel.hpp"

namespace radar::app::detection_model {

inline constexpr double kPi             = 3.14159265358979323846;
inline constexpr int    kRangeBins      = rf_model::kRangeBinCount;
inline constexpr double kRangeMinM      = rf_model::kMinimumReceiveRangeM;
inline constexpr double kRangeMaxM      = rf_model::kInstrumentedRangeM;
inline constexpr double kRangeResolutionM = rf_model::kRangeResolutionM;
inline constexpr double kNoiseSigma     = 0.05;
// RMS magnitude of complex noise whose I and Q axes each have kNoiseSigma.
inline constexpr double kNoiseMagnitudeRms =
    kNoiseSigma * 1.4142135623730950488;
inline constexpr double kCfarThreshold  = 0.26;

// The original demo sensitivity was calibrated at a nominal 10 cm
// wavelength. Monostatic received power is proportional to lambda^2, so
// voltage amplitude is proportional to lambda. Retain that calibration while
// making carrier-frequency changes affect received amplitude correctly.
inline constexpr double kReferenceWavelengthM = 0.10;
inline constexpr double kReferenceSignalScale = 3.0e8;
inline constexpr double kSignalScale =
    kReferenceSignalScale
    * (rf_model::kWavelengthM / kReferenceWavelengthM);

inline bool within_instrumented_range(double range_m) noexcept {
    return range_m >= kRangeMinM && range_m <= kRangeMaxM;
}

inline int range_bin_for(double range_m) noexcept {
    return static_cast<int>(range_m / kRangeResolutionM);
}

inline double range_m_for_bin(int bin) noexcept {
    return static_cast<double>(bin) * kRangeResolutionM;
}

inline double two_way_carrier_phase_rad(double range_m) noexcept {
    const double cycles = 2.0 * range_m / rf_model::kWavelengthM;
    return 2.0 * kPi * std::remainder(cycles, 1.0);
}

inline double compressed_pulse_weight(int bin_offset) noexcept {
    switch (bin_offset) {
        case 0:  return 1.0;
        case -1:
        case 1:  return 0.4;
        default: return 0.0;
    }
}

inline double target_voltage_amplitude(
        double rcs_dbsm,
        double range_m,
        double active_aperture_fraction,
        double pattern_response) noexcept {
    const double rcs_linear = std::pow(10.0, rcs_dbsm / 10.0);
    return kSignalScale
         * active_aperture_fraction
         * pattern_response
         * std::sqrt(rcs_linear)
         / (range_m * range_m);
}

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
