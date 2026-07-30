#pragma once
// Pulse-eclipse model for targets inside the normal receive blind range.
// It is enabled by default; radar_app can explicitly restore the hard gate.

#include <algorithm>
#include <cmath>

#include "DetectionModel.hpp"

namespace radar::app::near_range {

// Prevent an extremely close truth point from producing an unbounded
// normalized I/Q value in this far-field demo model. This clip is applied only
// to experimental truncated returns; ordinary >=3 km behavior is unchanged.
inline constexpr double kTruncatedReturnMaxVoltage = 64.0;

struct RangeObservation {
    bool observable = false;
    bool truncated = false;
    double true_range_m = 0.0;
    double apparent_range_m = 0.0;
    double coherent_fraction = 0.0;
};

inline RangeObservation observe_range(
        double true_range_m,
        bool sub_3km_enabled) noexcept {
    if (!std::isfinite(true_range_m)
        || true_range_m <= 0.0
        || true_range_m > detection_model::kRangeMaxM) {
        return {};
    }

    if (true_range_m >= detection_model::kRangeMinM) {
        return RangeObservation{
            true, false, true_range_m, true_range_m, 1.0};
    }

    if (!sub_3km_enabled)
        return {};

    // A rectangular echo delayed by tau overlaps the receive-open interval
    // for tau seconds after a pulse of width T. The coherent captured fraction
    // is therefore tau/T = true_range/minimum_receive_range.
    const double coherent_fraction = std::clamp(
        true_range_m / detection_model::kRangeMinM,
        0.0,
        1.0);

    // Correlating only the received tail produces a flat delay ambiguity from
    // the true delay through the transmit/receive boundary. Use its midpoint
    // as a deterministic representative peak. It is continuous at 3 km and
    // deliberately biased outward below that boundary.
    const double apparent_range_m =
        0.5 * (true_range_m + detection_model::kRangeMinM);
    return RangeObservation{
        coherent_fraction > 0.0,
        true,
        true_range_m,
        apparent_range_m,
        coherent_fraction};
}

inline double observed_target_voltage(
        double rcs_dbsm,
        const RangeObservation& observation,
        double active_aperture_fraction,
        double pattern_response) noexcept {
    if (!observation.observable)
        return 0.0;

    const double model_range_m = observation.truncated
        ? std::max(
              observation.true_range_m,
              0.5 * detection_model::kRangeResolutionM)
        : observation.true_range_m;
    const double voltage =
        detection_model::target_voltage_amplitude(
            rcs_dbsm,
            model_range_m,
            active_aperture_fraction,
            pattern_response)
        * observation.coherent_fraction;
    return observation.truncated
        ? std::min(voltage, kTruncatedReturnMaxVoltage)
        : voltage;
}

// The truncated rectangular-pulse correlation is ambiguous across
// [true_range, minimum_receive_range]. Given the selected midpoint, recover
// the modeled width and express a uniform plateau as one standard deviation.
inline double ambiguity_stddev_m(double reported_range_m) noexcept {
    if (!std::isfinite(reported_range_m)
        || reported_range_m >= detection_model::kRangeMinM) {
        return 0.0;
    }
    const double inferred_true_range_m = std::clamp(
        2.0 * reported_range_m - detection_model::kRangeMinM,
        0.0,
        detection_model::kRangeMinM);
    const double ambiguity_width_m =
        detection_model::kRangeMinM - inferred_true_range_m;
    return ambiguity_width_m / std::sqrt(12.0);
}

} // namespace radar::app::near_range
