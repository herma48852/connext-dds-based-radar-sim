#include "BeamPatternModel.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <limits>

namespace radar::app {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg2Rad = kPi / 180.0;
constexpr double kPatternFloorDb = -80.0;

// A 32-column half-wave ULA has a ~3.2-degree nominal HPBW.  The simulator's
// established beam is 2 degrees, so this effective-aperture scale preserves
// that baseline while retaining the outage-dependent array-factor shape.
constexpr double kEffectiveSpacingScale = 1.585;

double wrap180(double a) {
    while (a > 180.0) a -= 360.0;
    while (a < -180.0) a += 360.0;
    return a;
}

double array_amplitude(const std::array<int, 32>& column_weights,
                       int active_elements, double relative_deg) {
    if (active_elements <= 0)
        return 0.0;

    const double spatial_phase =
        kPi * kEffectiveSpacingScale * std::sin(relative_deg * kDeg2Rad);
    std::complex<double> sum{0.0, 0.0};
    for (int c = 0; c < 32; ++c) {
        const double x = c - 15.5;
        const double phase = x * spatial_phase;
        sum += static_cast<double>(column_weights[c])
             * std::complex<double>(std::cos(phase), std::sin(phase));
    }
    return std::clamp(std::abs(sum) / active_elements, 0.0, 1.0);
}

double amplitude_to_db(double amplitude) {
    return 20.0 * std::log10(std::max(amplitude, 1.0e-4));
}
} // namespace

double BeamPattern::relative_amplitude(double offset_deg) const {
    const double wrapped = wrap180(offset_deg);
    const double last = kBeamPatternStartDeg
                      + kBeamPatternStepDeg * (kBeamPatternSampleCount - 1);
    if (wrapped < kBeamPatternStartDeg || wrapped > last)
        return 0.0;

    const double f = (wrapped - kBeamPatternStartDeg) / kBeamPatternStepDeg;
    const int i0 = std::clamp(static_cast<int>(std::floor(f)), 0,
                              kBeamPatternSampleCount - 1);
    const int i1 = std::min(i0 + 1, kBeamPatternSampleCount - 1);
    const double t = f - i0;
    const double db = (1.0 - t) * azimuth_pattern_db[i0]
                    + t * azimuth_pattern_db[i1];
    return std::pow(10.0, db / 20.0);
}

BeamPattern BeamPatternModel::calculate(uint32_t rma_offline_mask) {
    BeamPattern result;
    result.rma_offline_mask = rma_offline_mask & 0xFFFFu;

    std::array<int, 32> column_weights{};
    double offline_horizontal_moment = 0.0;
    int active_elements = 0;

    for (int r = 0; r < 32; ++r) {
        for (int c = 0; c < 32; ++c) {
            const int rma = (r / 8) * 4 + (c / 8);
            if ((result.rma_offline_mask >> rma) & 1u)
                continue;
            ++column_weights[c];
            ++active_elements;
        }
    }
    for (int rma = 0; rma < 16; ++rma) {
        if ((result.rma_offline_mask >> rma) & 1u) {
            const int block_col = rma % 4;
            offline_horizontal_moment += block_col - 1.5;
        }
    }

    result.active_elements = active_elements;
    result.active_fraction = active_elements / 1024.0;
    // Keep all-offline telemetry finite for plots and monitoring. Receiver
    // synthesis treats zero active elements as exactly zero physical gain.
    result.gain_loss_db =
        20.0 * std::log10(std::max(0.01, result.active_fraction));

    // A dark amplitude aperture alone does not move the coherent peak.
    // This small deterministic term explicitly represents residual phase
    // calibration after the fallback beamformer removes an asymmetric RMA.
    // Mirrored/symmetric outages cancel; the error is deliberately bounded.
    result.boresight_error_deg = std::clamp(
        -0.18 * offline_horizontal_moment, -1.5, 1.5);

    if (active_elements == 0) {
        result.beamwidth_3db_deg = 90.0;
        result.peak_sidelobe_level_db = kPatternFloorDb;
        result.left_sidelobe_offset_deg = 0.0;
        result.right_sidelobe_offset_deg = 0.0;
        result.azimuth_pattern_db.fill(
            static_cast<float>(kPatternFloorDb));
        return result;
    }

    for (int i = 0; i < kBeamPatternSampleCount; ++i) {
        const double offset =
            kBeamPatternStartDeg + i * kBeamPatternStepDeg;
        const double relative = offset - result.boresight_error_deg;
        result.azimuth_pattern_db[i] = static_cast<float>(
            std::max(kPatternFloorDb,
                     amplitude_to_db(array_amplitude(
                         column_weights, active_elements, relative))));
    }

    // High-resolution half-power crossing around the coherent main lobe.
    constexpr double kHalfPowerAmplitude = 0.7071067811865476;
    double previous_offset = 0.0;
    double previous_amplitude = 1.0;
    double half_power_offset = 1.0;
    for (double offset = 0.01; offset <= 20.0; offset += 0.01) {
        const double amplitude =
            array_amplitude(column_weights, active_elements, offset);
        if (amplitude <= kHalfPowerAmplitude) {
            const double span = previous_amplitude - amplitude;
            const double t = span > 1.0e-12
                ? (previous_amplitude - kHalfPowerAmplitude) / span : 0.0;
            half_power_offset =
                previous_offset + t * (offset - previous_offset);
            break;
        }
        previous_offset = offset;
        previous_amplitude = amplitude;
    }
    result.beamwidth_3db_deg = 2.0 * half_power_offset;

    // Find the strongest local sidelobe independently on each side.  The
    // exclusion is wider than the 3 dB lobe so its shoulders cannot win.
    double left_db = -std::numeric_limits<double>::infinity();
    double right_db = -std::numeric_limits<double>::infinity();
    const double exclusion = std::max(2.0, result.beamwidth_3db_deg);
    for (int i = 1; i < kBeamPatternSampleCount - 1; ++i) {
        const double offset =
            kBeamPatternStartDeg + i * kBeamPatternStepDeg;
        if (std::fabs(offset - result.boresight_error_deg) < exclusion)
            continue;
        const double db = result.azimuth_pattern_db[i];
        if (db < result.azimuth_pattern_db[i - 1] ||
            db < result.azimuth_pattern_db[i + 1])
            continue;
        if (offset < result.boresight_error_deg && db > left_db) {
            left_db = db;
            result.left_sidelobe_offset_deg = offset;
        } else if (offset > result.boresight_error_deg && db > right_db) {
            right_db = db;
            result.right_sidelobe_offset_deg = offset;
        }
    }

    const double peak = std::max(left_db, right_db);
    result.peak_sidelobe_level_db =
        std::isfinite(peak) ? peak : kPatternFloorDb;
    return result;
}

} // namespace radar::app
