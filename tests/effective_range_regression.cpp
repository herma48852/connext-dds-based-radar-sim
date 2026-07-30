#include "EffectiveRangeModel.hpp"

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

bool near(double actual, double expected, double tolerance) {
    return std::fabs(actual - expected) <= tolerance;
}
} // namespace

int main() {
    namespace effective = radar::app::effective_range;
    namespace detection = radar::app::detection_model;

    check(near(
              effective::detection_threshold_reported_snr_db(),
              11.31,
              0.02),
          "fixed detector threshold has an approximately 11.3 dB report floor");
    check(near(
              effective::threshold_target_voltage(),
              0.2502,
              0.0002),
          "threshold-crossing target voltage includes the complex-noise RMS");

    struct SensitivityCase {
        double rcs_dbsm;
        double expected_km;
    };
    constexpr SensitivityCase sensitivity_cases[]{
        {-15.0, 14.6},
        {-10.0, 19.5},
        {  0.0, 34.6},
        {  5.0, 46.2},
        { 20.0, 109.5},
        { 35.0, 259.6},
    };
    for (const auto& test : sensitivity_cases) {
        const double range_km =
            effective::nominal_threshold_crossing_range_m(
                test.rcs_dbsm) / 1000.0;
        check(near(range_km, test.expected_km, 0.15),
              "beam-center sensitivity table remains calibrated");
    }
    check(near(
              effective::effective_detection_range_m(20.0),
              detection::kRangeMaxM,
              1.0e-9),
          "effective range is capped by the 100 km instrumented range");

    const auto threshold_uncertainty =
        effective::measurement_uncertainty(
            effective::detection_threshold_reported_snr_db());
    const auto strong_uncertainty =
        effective::measurement_uncertainty(40.0);
    check(threshold_uncertainty.range_stddev_m
              > strong_uncertainty.range_stddev_m,
          "range uncertainty decreases with S/N");
    check(threshold_uncertainty.azimuth_stddev_deg
              > strong_uncertainty.azimuth_stddev_deg,
          "azimuth uncertainty decreases with S/N");
    check(near(
              strong_uncertainty.range_stddev_m,
              detection::kRangeResolutionM / std::sqrt(12.0),
              0.1),
          "strong-return range uncertainty approaches its quantization floor");
    check(near(
              strong_uncertainty.azimuth_stddev_deg,
              radar::app::search_raster::kAzimuthStepDeg
                  / std::sqrt(12.0),
              0.01),
          "strong-return azimuth uncertainty approaches its raster floor");
    check(near(
              threshold_uncertainty.elevation_stddev_deg,
              11.0 / std::sqrt(12.0),
              1.0e-12),
          "bar-only elevation uncertainty remains the 11-degree interval floor");

    const auto covariance =
        effective::cartesian_position_covariance(
            20000.0,
            0.0,
            14.0,
            threshold_uncertainty);
    const auto east_covariance =
        effective::cartesian_position_covariance(
            20000.0,
            90.0,
            14.0,
            threshold_uncertainty);
    check(near(covariance[0], east_covariance[4], 1.0e-6)
              && near(covariance[4], east_covariance[0], 1.0e-6),
          "rotating bearing by 90 degrees rotates east/north covariance");
    check(covariance[8] > covariance[0],
          "coarse elevation bars dominate vertical position uncertainty");
    check(near(covariance[1], covariance[3], 1.0e-9)
              && near(covariance[2], covariance[6], 1.0e-9)
              && near(covariance[5], covariance[7], 1.0e-9),
          "Cartesian covariance is symmetric");
    check(covariance[0] > 0.0
              && covariance[4] > 0.0
              && covariance[8] > 0.0,
          "Cartesian covariance has positive axis variances");

    if (failures != 0) {
        std::fprintf(
            stderr,
            "effective_range_regression: %d failure(s)\n",
            failures);
        return 1;
    }
    std::printf(
        "effective_range_regression: PASS "
        "(threshold %.2f dB, -15 dBsm %.1f km)\n",
        effective::detection_threshold_reported_snr_db(),
        effective::nominal_threshold_crossing_range_m(-15.0)
            / 1000.0);
    return 0;
}
