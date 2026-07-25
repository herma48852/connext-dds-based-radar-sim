#pragma once
// ============================================================================
// Representative, unclassified RF and search-waveform model.
//
// SPY-6 is an S-band radar. Its exact operating waveform is not modeled or
// claimed here; these public-band, round-number values provide one internally
// consistent physical basis for antenna spacing, carrier phase, pulse timing,
// and range sampling throughout the demo.
// ============================================================================

namespace radar::app::rf_model {

inline constexpr double kSpeedOfLightMps = 299792458.0;

// Representative S-band search carrier: 3 GHz -> approximately 10 cm.
inline constexpr double kCarrierFrequencyHz = 3.0e9;
inline constexpr double kWavelengthM =
    kSpeedOfLightMps / kCarrierFrequencyHz;

// Fixed physical pitch. At the representative carrier this is very nearly
// half a wavelength; keeping it in metres means frequency changes correctly
// change d/lambda and therefore the array factor.
inline constexpr double kElementPitchM = 0.050;
inline constexpr double kElementSpacingWavelengths =
    kElementPitchM / kWavelengthM;

// Representative pulse-compressed search waveform.
inline constexpr double kWaveformBandwidthHz = 1.0e6;
inline constexpr double kPulseRepetitionFrequencyHz = 1000.0;
inline constexpr double kPulseWidthSec = 20.0e-6;

inline constexpr double kRangeResolutionM =
    kSpeedOfLightMps / (2.0 * kWaveformBandwidthHz);
inline constexpr double kUnambiguousRangeM =
    kSpeedOfLightMps / (2.0 * kPulseRepetitionFrequencyHz);
inline constexpr double kMinimumReceiveRangeM =
    kSpeedOfLightMps * kPulseWidthSec / 2.0;
inline constexpr double kDutyCycle =
    kPulseWidthSec * kPulseRepetitionFrequencyHz;

inline constexpr double kInstrumentedRangeM = 100000.0;

constexpr int ceil_positive(double value) noexcept {
    const int truncated = static_cast<int>(value);
    return static_cast<double>(truncated) < value
        ? truncated + 1 : truncated;
}

// One complex I/Q cell per bandwidth-limited range-resolution interval.
inline constexpr int kRangeBinCount =
    ceil_positive(kInstrumentedRangeM / kRangeResolutionM);
inline constexpr int kIqScalarsPerReturn = 2 * kRangeBinCount;

// Must match radar::types::MAX_RANGE_BINS in idl/radar_types.idl. The legacy
// IDL name denotes scalar sequence capacity, not complex range-cell count.
inline constexpr int kMaxIqScalarsPerReturn = 2048;

static_assert(kCarrierFrequencyHz >= 2.0e9
              && kCarrierFrequencyHz <= 4.0e9);
static_assert(kElementSpacingWavelengths > 0.49
              && kElementSpacingWavelengths < 0.51);
static_assert(kUnambiguousRangeM > kInstrumentedRangeM);
static_assert(kDutyCycle > 0.0 && kDutyCycle < 1.0);
static_assert(kIqScalarsPerReturn <= kMaxIqScalarsPerReturn);

} // namespace radar::app::rf_model
