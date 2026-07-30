#include "DetectionModel.hpp"
#include "RadarRfModel.hpp"

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
    namespace detection = radar::app::detection_model;
    namespace rf = radar::app::rf_model;

    check(rf::kCarrierFrequencyHz >= 2.0e9
              && rf::kCarrierFrequencyHz <= 4.0e9,
          "representative carrier is in S band");
    check(rf::kWavelengthM > 0.09 && rf::kWavelengthM < 0.11,
          "3 GHz carrier derives an approximately 10 cm wavelength");
    check(std::fabs(
              rf::kWavelengthM * rf::kCarrierFrequencyHz
              - rf::kSpeedOfLightMps) < 1.0e-6,
          "carrier and wavelength are related by the speed of light");
    check(rf::kElementSpacingWavelengths > 0.49
              && rf::kElementSpacingWavelengths < 0.51,
          "50 mm physical pitch is approximately half a wavelength");

    check(rf::kRangeResolutionM > 149.0
              && rf::kRangeResolutionM < 151.0,
          "1 MHz bandwidth derives approximately 150 m range resolution");
    check(rf::kUnambiguousRangeM > rf::kInstrumentedRangeM,
          "1 kHz PRF has unambiguous range beyond the 100 km display");
    check(rf::kMinimumReceiveRangeM > 2990.0
              && rf::kMinimumReceiveRangeM < 3000.0,
          "20 microsecond pulse derives an approximately 3 km blind range");
    check(std::fabs(rf::kDutyCycle - 0.02) < 1.0e-12,
          "pulse width and PRF derive a 2 percent duty cycle");
    check(rf::kRangeBinCount == 668,
          "100 km instrumented range requires 668 resolution cells");
    check(rf::kIqScalarsPerReturn == 1336
              && rf::kIqScalarsPerReturn <= rf::kMaxIqScalarsPerReturn,
          "interleaved complex cells fit the bounded DDS sequence");

    const double test_range_m = 42123.0;
    const int test_bin = detection::range_bin_for(test_range_m);
    const double quantized_range_m = detection::range_m_for_bin(test_bin);
    check(std::fabs(quantized_range_m - test_range_m)
              <= rf::kRangeResolutionM * 0.5,
          "range conversion reports the center of the bandwidth-derived cell");
    check(!detection::within_instrumented_range(
              rf::kMinimumReceiveRangeM - 1.0)
              && detection::within_instrumented_range(
                  rf::kMinimumReceiveRangeM),
          "pulse blind-range boundary is enforced");
    check(detection::within_instrumented_range(
              rf::kInstrumentedRangeM)
              && !detection::within_instrumented_range(
                  rf::kInstrumentedRangeM + 1.0),
          "instrumented-range boundary is enforced");

    const double phase0 =
        detection::two_way_carrier_phase_rad(test_range_m);
    const double phase1 = detection::two_way_carrier_phase_rad(
        test_range_m + rf::kWavelengthM * 0.5);
    check(std::fabs(std::remainder(phase1 - phase0, 2.0 * detection::kPi))
              < 1.0e-8,
          "two-way carrier phase repeats after half-wavelength range change");

    const double amp_near = detection::target_voltage_amplitude(
        0.0, 20000.0, 1.0, 1.0);
    const double amp_far = detection::target_voltage_amplitude(
        0.0, 40000.0, 1.0, 1.0);
    check(std::fabs(amp_near / amp_far - 4.0) < 1.0e-12,
          "voltage amplitude follows inverse range squared");

    if (failures != 0) {
        std::fprintf(stderr,
                     "radar_rf_model_regression: %d failure(s)\n",
                     failures);
        return 1;
    }
    std::printf(
        "radar_rf_model_regression: PASS "
        "(%.1f GHz, %.2f cm, %.1f m cells, %d bins)\n",
        rf::kCarrierFrequencyHz / 1.0e9,
        rf::kWavelengthM * 100.0,
        rf::kRangeResolutionM,
        rf::kRangeBinCount);
    return 0;
}
