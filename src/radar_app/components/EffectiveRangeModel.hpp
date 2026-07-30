#pragma once
// Effective-range measurement model for the simplified search radar.
//
// This file does not change detection probability or the receiver blind
// range. It characterizes the measurements already emitted by the fixed
// threshold detector and provides uncertainty estimates for association and
// TargetTrack covariance.

#include <algorithm>
#include <array>
#include <cmath>

#include "DetectionModel.hpp"
#include "SearchRaster.hpp"

namespace radar::app::effective_range {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kDeg2Rad = kPi / 180.0;

// The nominal 32-element half-wavelength aperture has an approximately
// 3.17-degree azimuth half-power beamwidth. Detection azimuth is still a
// raster-bearing estimate, not a monopulse angle.
inline constexpr double kNominalAzimuthBeamwidthDeg = 3.1719;

// Adjacent elevation-bar centers are 11 degrees apart. Each bar therefore
// represents an interval extending halfway to the neighboring bar.
inline constexpr double kElevationBarHalfWidthDeg =
    (search_raster::kElevationBarsDeg[1]
     - search_raster::kElevationBarsDeg[0]) * 0.5;

// CoreDetection is also used by small offline tests that do not synthesize a
// return amplitude. Twelve dB is just above the production fixed-threshold
// report floor (approximately 11.3 dB).
inline constexpr double kDefaultReportedSnrDb = 12.0;

struct MeasurementUncertainty {
    double range_stddev_m;
    double azimuth_stddev_deg;
    double elevation_stddev_deg;
};

using CartesianCovariance = std::array<double, 9>;

inline double detection_threshold_reported_snr_db() noexcept {
    return 20.0 * std::log10(
        detection_model::kCfarThreshold
        / detection_model::kNoiseMagnitudeRms);
}

// DetectionEvent.snr_db is the integrated magnitude divided by the complex
// noise RMS, expressed with 20*log10. Squaring that amplitude ratio produces
// the power ratio used by the uncertainty model.
inline double reported_power_ratio(double reported_snr_db) noexcept {
    const double finite_snr_db =
        std::isfinite(reported_snr_db)
        ? reported_snr_db : kDefaultReportedSnrDb;
    const double accepted_snr_db = std::clamp(
        finite_snr_db,
        detection_threshold_reported_snr_db(),
        80.0);
    return std::pow(10.0, accepted_snr_db / 10.0);
}

inline MeasurementUncertainty measurement_uncertainty(
        double reported_snr_db) noexcept {
    const double power_ratio = reported_power_ratio(reported_snr_db);
    const double sqrt_power_ratio = std::sqrt(power_ratio);

    // Uniform range-cell and raster-bearing quantization provide irreducible
    // floors. The added terms are conservative inverse-SNR centroiding
    // approximations; independent contributions combine root-sum-square.
    const double range_quantization =
        detection_model::kRangeResolutionM / std::sqrt(12.0);
    const double range_noise =
        detection_model::kRangeResolutionM
        / (2.0 * sqrt_power_ratio);
    const double azimuth_quantization =
        search_raster::kAzimuthStepDeg / std::sqrt(12.0);
    const double azimuth_noise =
        kNominalAzimuthBeamwidthDeg
        / (2.0 * std::sqrt(2.0 * power_ratio));
    const double elevation_quantization =
        (2.0 * kElevationBarHalfWidthDeg) / std::sqrt(12.0);

    return MeasurementUncertainty{
        std::hypot(range_quantization, range_noise),
        std::hypot(azimuth_quantization, azimuth_noise),
        elevation_quantization};
}

// Approximate the mean integrated RMS magnitude of a coherent target in
// independent I/Q thermal noise. This is an engineering approximation to the
// Rician/noncoherent detector, not a probability-of-detection calculation.
inline double expected_integrated_magnitude(
        double target_voltage) noexcept {
    return std::hypot(
        target_voltage,
        detection_model::kNoiseMagnitudeRms);
}

inline double threshold_target_voltage() noexcept {
    const double threshold_squared =
        detection_model::kCfarThreshold
        * detection_model::kCfarThreshold;
    const double noise_squared =
        detection_model::kNoiseMagnitudeRms
        * detection_model::kNoiseMagnitudeRms;
    return std::sqrt(std::max(0.0, threshold_squared - noise_squared));
}

// Nominal beam-center threshold crossing before the 100 km instrumented-range
// cap. Range scales as RCS^(1/4), because voltage is sqrt(RCS)/range^2.
inline double nominal_threshold_crossing_range_m(
        double rcs_dbsm,
        double active_aperture_fraction = 1.0,
        double pattern_response = 1.0) noexcept {
    const double usable_gain =
        std::max(0.0, active_aperture_fraction)
        * std::max(0.0, pattern_response);
    const double required_voltage = threshold_target_voltage();
    if (usable_gain == 0.0 || required_voltage == 0.0)
        return 0.0;
    const double rcs_linear = std::pow(10.0, rcs_dbsm / 10.0);
    return std::sqrt(
        detection_model::kSignalScale
        * usable_gain
        * std::sqrt(rcs_linear)
        / required_voltage);
}

inline double effective_detection_range_m(
        double rcs_dbsm,
        double active_aperture_fraction = 1.0,
        double pattern_response = 1.0) noexcept {
    return std::min(
        detection_model::kRangeMaxM,
        nominal_threshold_crossing_range_m(
            rcs_dbsm,
            active_aperture_fraction,
            pattern_response));
}

// Transform independent [range, azimuth, elevation] measurement variances to
// a full row-major ENU position covariance with the spherical-coordinate
// Jacobian. Angles supplied to the Jacobian are in degrees; their variances
// are converted to radians squared.
inline CartesianCovariance cartesian_position_covariance(
        double range_m,
        double azimuth_world_deg,
        double elevation_deg,
        const MeasurementUncertainty& uncertainty) noexcept {
    const double az = azimuth_world_deg * kDeg2Rad;
    const double el = elevation_deg * kDeg2Rad;
    const double sin_az = std::sin(az);
    const double cos_az = std::cos(az);
    const double sin_el = std::sin(el);
    const double cos_el = std::cos(el);

    const double range_variance =
        uncertainty.range_stddev_m * uncertainty.range_stddev_m;
    const double azimuth_variance =
        std::pow(uncertainty.azimuth_stddev_deg * kDeg2Rad, 2.0);
    const double elevation_variance =
        std::pow(uncertainty.elevation_stddev_deg * kDeg2Rad, 2.0);

    const std::array<double, 3> dr{
        cos_el * sin_az,
        cos_el * cos_az,
        sin_el};
    const std::array<double, 3> da{
        range_m * cos_el * cos_az,
        -range_m * cos_el * sin_az,
        0.0};
    const std::array<double, 3> de{
        -range_m * sin_el * sin_az,
        -range_m * sin_el * cos_az,
        range_m * cos_el};

    CartesianCovariance covariance{};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            covariance[3 * row + column] =
                range_variance * dr[row] * dr[column]
                + azimuth_variance * da[row] * da[column]
                + elevation_variance * de[row] * de[column];
        }
    }
    return covariance;
}

} // namespace radar::app::effective_range
