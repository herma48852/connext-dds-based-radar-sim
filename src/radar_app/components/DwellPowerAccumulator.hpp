#pragma once
// Noncoherent pulse integration for one post-beamforming receiver stream.
//
// A search dwell contains ten PRF samples. Treating every pulse threshold
// crossing as an independent plot inflates tracker hit counts and allows a
// single illumination to confirm a track. This accumulator averages I/Q
// power across the complete dwell and exposes one RMS-magnitude range trace.
// Noncoherent integration is intentional: the demo does not yet implement a
// Doppler filter bank that could phase-align a moving target coherently.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace radar::app {

class DwellPowerAccumulator {
public:
    void begin(int64_t beam_id,
               double azimuth_deg,
               double elevation_deg,
               int range_bin_count) {
        beam_id_ = beam_id;
        azimuth_deg_ = azimuth_deg;
        elevation_deg_ = elevation_deg;
        range_bin_count_ = std::max(0, range_bin_count);
        pulse_count_ = 0;
        active_ = true;
        power_sum_.assign(
            static_cast<std::size_t>(range_bin_count_), 0.0);
    }

    bool active() const noexcept { return active_; }
    bool matches(int64_t beam_id) const noexcept {
        return active_ && beam_id_ == beam_id;
    }

    int64_t beam_id() const noexcept { return beam_id_; }
    double azimuth_deg() const noexcept { return azimuth_deg_; }
    double elevation_deg() const noexcept { return elevation_deg_; }
    int range_bin_count() const noexcept { return range_bin_count_; }
    int pulse_count() const noexcept { return pulse_count_; }

    void accumulate(std::span<const float> interleaved_iq) {
        if (!active_)
            return;
        const int available_bins =
            static_cast<int>(interleaved_iq.size() / 2);
        const int bins = std::min(range_bin_count_, available_bins);
        for (int i = 0; i < bins; ++i) {
            const double in_phase = interleaved_iq[2 * i];
            const double quadrature = interleaved_iq[2 * i + 1];
            power_sum_[static_cast<std::size_t>(i)] +=
                in_phase * in_phase + quadrature * quadrature;
        }
        ++pulse_count_;
    }

    // Completes the dwell and resets the accumulator. Returns false for an
    // empty dwell, which can occur only during startup or malformed input.
    bool complete(std::vector<float>& rms_magnitude) {
        if (!active_ || pulse_count_ <= 0) {
            clear();
            return false;
        }

        rms_magnitude.resize(
            static_cast<std::size_t>(range_bin_count_));
        const double inverse_count =
            1.0 / static_cast<double>(pulse_count_);
        for (int i = 0; i < range_bin_count_; ++i) {
            rms_magnitude[static_cast<std::size_t>(i)] =
                static_cast<float>(std::sqrt(
                    power_sum_[static_cast<std::size_t>(i)]
                    * inverse_count));
        }
        clear();
        return true;
    }

    void clear() noexcept {
        active_ = false;
        pulse_count_ = 0;
        range_bin_count_ = 0;
    }

private:
    bool active_{false};
    int64_t beam_id_{-1};
    double azimuth_deg_{0.0};
    double elevation_deg_{0.0};
    int range_bin_count_{0};
    int pulse_count_{0};
    std::vector<double> power_sum_;
};

} // namespace radar::app
