#include "DetectionProcessor.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <memory>

#include "Log.hpp"
#include "SimClock.hpp"

namespace radar::app {

namespace {
constexpr double kDeg2Rad = 3.14159265358979323846 / 180.0;

double wrap180(double a) {
    while (a > 180.0)  a -= 360.0;
    while (a < -180.0) a += 360.0;
    return a;
}

// Generic listener adapter: forwards whole loaned batches to a member
// function. Runs on the DDS receive thread -> keep the handler light.
template <typename T, typename Owner, void (Owner::*Method)(const T&)>
class ForwardingListener : public dds::sub::NoOpDataReaderListener<T> {
public:
    explicit ForwardingListener(Owner* owner) : owner_(owner) {}
    void on_data_available(dds::sub::DataReader<T>& reader) override {
        T sample;
        dds::sub::SampleInfo info;
        for (int i = 0;
             i < 256 && reader.extensions().take(sample, info); ++i) {
            if (info.valid())
                (owner_->*Method)(sample);
        }
    }
private:
    Owner* owner_;
};
} // namespace

void DetectionProcessor::start() {
    auto beam_topic  = radds::make_topic<types::BeamCommand>(participant_, dds_names::TOPIC_BEAM_COMMAND);
    auto raw_topic   = radds::make_topic<types::RawReturn>(participant_, dds_names::TOPIC_RAW_RETURN);
    auto det_topic   = radds::make_topic<types::DetectionEvent>(participant_, dds_names::TOPIC_DETECTION_EVENT);
    auto pattern_topic = radds::make_topic<types::BeamPatternStatus>(
        participant_, dds_names::TOPIC_BEAM_PATTERN_STATUS);
    auto truth_topic = radds::make_topic<types::TargetTruth>(participant_, dds_names::TOPIC_TARGET_TRUTH);

    raw_writer_   = radds::make_writer<types::RawReturn>(publisher_, raw_topic, dds_names::PROFILE_RAW_RETURN);
    det_writer_   = radds::make_writer<types::DetectionEvent>(publisher_, det_topic, dds_names::PROFILE_DETECTION_EVENT);
    pattern_writer_ = radds::make_writer<types::BeamPatternStatus>(
        publisher_, pattern_topic, dds_names::PROFILE_BEAM_PATTERN_STATUS);
    beam_reader_  = radds::make_reader<types::BeamCommand>(subscriber_, beam_topic, dds_names::PROFILE_BEAM_COMMAND);
    raw_reader_   = radds::make_reader<types::RawReturn>(subscriber_, raw_topic, dds_names::PROFILE_RAW_RETURN);
    truth_reader_ = radds::make_reader<types::TargetTruth>(subscriber_, truth_topic, dds_names::PROFILE_TARGET_TRUTH);

    // Listeners for the high-rate paths (BeamCommand, RawReturn, TargetTruth).
    beam_reader_.set_listener(
        std::make_shared<ForwardingListener<types::BeamCommand, DetectionProcessor,
                                            &DetectionProcessor::on_beam_command>>(this),
        dds::core::status::StatusMask::data_available());
    raw_reader_.set_listener(
        std::make_shared<ForwardingListener<types::RawReturn, DetectionProcessor,
                                            &DetectionProcessor::on_raw_return>>(this),
        dds::core::status::StatusMask::data_available());
    truth_reader_.set_listener(
        std::make_shared<ForwardingListener<types::TargetTruth, DetectionProcessor,
                                            &DetectionProcessor::on_truth>>(this),
        dds::core::status::StatusMask::data_available());

    spawn([this] { return_synthesis_loop(); });
}

void DetectionProcessor::stop() {
    stop_.store(true);
    detach_listener(beam_reader_);
    detach_listener(raw_reader_);
    detach_listener(truth_reader_);
    join_all();
}

void DetectionProcessor::on_beam_command(const types::BeamCommand& cmd) {
    dwell_beam_id_.store(cmd.beam_id);
    dwell_az_deg_.store(cmd.azimuth_deg);
    dwell_el_deg_.store(cmd.elevation_deg);
}

void DetectionProcessor::on_truth(const types::TargetTruth& t) {
    std::lock_guard lk(truth_mutex_);
    auto& s = truth_[t.target_id];
    s.x  = t.position.x_east_m;  s.y  = t.position.y_north_m;  s.z  = t.position.z_up_m;
    s.vx = t.velocity.x_east_m;  s.vy = t.velocity.y_north_m;  s.vz = t.velocity.z_up_m;
    s.rcs_dbsm    = t.rcs_dbsm;
    s.target_type = static_cast<int32_t>(t.target_type);
    s.last_sim_ms = t.timestamp.sim_millis;
}

// --- Receiver simulation: 1 kHz RawReturn synthesis for the current dwell --
void DetectionProcessor::return_synthesis_loop() {
    using namespace std::chrono;
    auto next = steady_clock::now();

    types::RawReturn sample;
    sample.range_bin_count = kRangeBins;
    sample.iq_samples.resize(2 * kRangeBins);

    types::BeamPatternStatus pattern_status;
    pattern_status.array_id = 0;
    pattern_status.azimuth_pattern_db.resize(kBeamPatternSampleCount);
    uint32_t cached_rma_mask = 0xFFFFFFFFu;
    BeamPattern pattern;
    auto next_pattern_publish = steady_clock::now();

    while (!stop_.load()) {
        next += milliseconds(1); // 1 kHz simulated PRF

        const int64_t beam_id = dwell_beam_id_.load();
        if (beam_id < 0) { // no dwell scheduled yet
            std::this_thread::sleep_until(next);
            continue;
        }
        const double az_deg = dwell_az_deg_.load();
        const double el_deg = dwell_el_deg_.load();

        // RMA-offline effect (Tier-1 physics): array gain ~ N_active,
        // azimuth beamwidth ~ 1/sqrt(N_active) (aperture shrink). Floor is
        // 1%, NOT 10%: at 10% the 35 dBsm ship still clears CFAR at 50 km
        // (amp 0.45 > 0.26) — "array offline" must actually go dark.
        // Elevation gate untouched — the 3-bar raster tiling depends on it.
        const uint32_t rma_mask = bus_.rma_offline_mask.load() & 0xFFFFu;
        if (rma_mask != cached_rma_mask) {
            pattern = BeamPatternModel::calculate(rma_mask);
            cached_rma_mask = rma_mask;
            RADAR_LOG << "[DetectionProcessor] beam pattern mask="
                      << pattern.rma_offline_mask
                      << " active=" << pattern.active_elements
                      << " loss_db=" << pattern.gain_loss_db
                      << " bw_deg=" << pattern.beamwidth_3db_deg
                      << " psl_db=" << pattern.peak_sidelobe_level_db
                      << " error_deg=" << pattern.boresight_error_deg
                      << "\n";
        }
        const double active = std::max(0.01, pattern.active_fraction);
        const double az_half_beam = kBeamwidthDeg * 0.5 / std::sqrt(active);

        // Publish scalar metrics plus the normalized azimuth cut at 20 Hz.
        // The transient-local profile lets Studio or HMI-UI join late and
        // immediately receive the latest degradation state.
        const auto pattern_now = steady_clock::now();
        if (pattern_now >= next_pattern_publish) {
            next_pattern_publish = pattern_now + milliseconds(50);
            pattern_status.timestamp = SimClock::stamp();
            pattern_status.beam_id = static_cast<int32_t>(beam_id);
            pattern_status.rma_offline_mask =
                static_cast<int32_t>(pattern.rma_offline_mask);
            pattern_status.commanded_azimuth_deg = az_deg;
            pattern_status.boresight_error_deg =
                pattern.boresight_error_deg;
            pattern_status.gain_loss_db = pattern.gain_loss_db;
            pattern_status.beamwidth_3db_deg = pattern.beamwidth_3db_deg;
            pattern_status.peak_sidelobe_level_db =
                pattern.peak_sidelobe_level_db;
            pattern_status.left_sidelobe_offset_deg =
                pattern.left_sidelobe_offset_deg;
            pattern_status.right_sidelobe_offset_deg =
                pattern.right_sidelobe_offset_deg;
            pattern_status.pattern_start_offset_deg =
                kBeamPatternStartDeg;
            pattern_status.pattern_step_deg = kBeamPatternStepDeg;
            for (int i = 0; i < kBeamPatternSampleCount; ++i)
                pattern_status.azimuth_pattern_db[i] =
                    pattern.azimuth_pattern_db[i];
            pattern_writer_.write(pattern_status);
        }

        // Noise floor
        for (int i = 0; i < 2 * kRangeBins; ++i)
            sample.iq_samples[i] = noise_(rng_);

        // Implant targets inside the beam (snapshot truth under lock)
        {
            std::lock_guard lk(truth_mutex_);
            const double heading = bus_.ship().heading_deg;
            for (const auto& [id, t] : truth_) {
                const double range_xy = std::hypot(t.x, t.y);
                const double range    = std::sqrt(t.x*t.x + t.y*t.y + t.z*t.z);
                if (range < 100.0 || range > kRangeMaxM) continue;

                // world ENU azimuth -> ship-relative azimuth
                const double az_world = std::atan2(t.x, t.y) / kDeg2Rad;
                const double az_ship  = wrap180(az_world - heading);
                const double beam_offset = wrap180(az_ship - az_deg);

                double pattern_response = 1.0;
                if (rma_mask == 0) {
                    // Preserve the established nominal regression exactly.
                    if (std::fabs(beam_offset) > az_half_beam)
                        continue;
                } else {
                    const double main_offset =
                        wrap180(beam_offset - pattern.boresight_error_deg);
                    const double main_half_width = std::max(
                        az_half_beam, pattern.beamwidth_3db_deg * 0.5);
                    if (std::fabs(main_offset) <= main_half_width) {
                        pattern_response =
                            pattern.relative_amplitude(beam_offset);
                    } else {
                        // Bound sidelobe theatre to the two dominant lobes.
                        // The extra -9 dB scale keeps ordinary air targets
                        // quiet while allowing a strong ship-sized return to
                        // make an occasional, explainable displaced ghost.
                        constexpr double kSidelobeDwellGateDeg = 1.2;
                        constexpr double kSidelobeTheatreScale = 0.35;
                        const bool pattern_has_sidelobe =
                            pattern.active_elements > 0 &&
                            pattern.peak_sidelobe_level_db > -30.0;
                        const bool near_left = pattern_has_sidelobe &&
                            std::fabs(wrap180(
                                beam_offset
                                - pattern.left_sidelobe_offset_deg))
                                <= kSidelobeDwellGateDeg;
                        const bool near_right = pattern_has_sidelobe &&
                            std::fabs(wrap180(
                                beam_offset
                                - pattern.right_sidelobe_offset_deg))
                                <= kSidelobeDwellGateDeg;
                        if (!near_left && !near_right)
                            continue;
                        pattern_response =
                            pattern.relative_amplitude(beam_offset)
                            * kSidelobeTheatreScale;
                    }
                }

                const double el_t = std::atan2(t.z, range_xy) / kDeg2Rad;
                // Elevation gate: +/-5.5 deg so the scheduler's 3/14 deg
                // bars tile WITHOUT overlap (-2.5..8.5 / 8.5..19.5) — an
                // overlapping target would alternate bars sweep-to-sweep
                // and its reported z (= R sin(bar_el)) would jump by km.
                if (std::fabs(el_t - el_deg) > 5.5)
                    continue; // outside elevation beam

                const double rcs_lin = std::pow(10.0, t.rcs_dbsm / 10.0);
                const double amp =
                    kSignalScale * active * pattern_response
                    * std::sqrt(rcs_lin) / (range * range);

                const double bin_f = range / kRangeMaxM * kRangeBins;
                const int b0 = static_cast<int>(bin_f);
                // spread the return over ~3 bins (matched-filter response)
                for (int db = -1; db <= 1; ++db) {
                    const int b = b0 + db;
                    if (b < 0 || b >= kRangeBins) continue;
                    const double w = (db == 0) ? 1.0 : 0.4;
                    sample.iq_samples[2*b]   += static_cast<float>(amp * w);
                    sample.iq_samples[2*b+1] += static_cast<float>(amp * w * 0.3);
                }
            }
        }

        sample.beam_id        = beam_id;
        sample.timestamp      = SimClock::stamp();
        sample.azimuth_deg    = az_deg;
        sample.elevation_deg  = el_deg;
        raw_writer_.write(sample);

        std::this_thread::sleep_until(next);
    }
}

// --- Signal processor: CFAR threshold crossings -> DetectionEvent ----------
void DetectionProcessor::on_raw_return(const types::RawReturn& ret) {
    // Bound by the ACTUAL sequence length, not just the declared bin count:
    // a foreign/malformed publisher could send range_bin_count = N with a
    // shorter iq_samples buffer, and indexing would run off the end.
    const int n = std::min<int>(
        std::min<int>(ret.range_bin_count, kRangeBins),
        static_cast<int>(ret.iq_samples.size()) / 2);
    if (n < 3) return;

    // magnitude per bin (also feeds the A-scope trace)
    static thread_local std::vector<float> mag;
    mag.resize(n);
    for (int i = 0; i < n; ++i) {
        const float I = ret.iq_samples[2*i];
        const float Q = ret.iq_samples[2*i+1];
        mag[i] = std::sqrt(I*I + Q*Q);
    }

    TraceBuffer tb;
    tb.magnitude     = mag;
    tb.azimuth_deg   = ret.azimuth_deg;
    tb.elevation_deg = ret.elevation_deg;
    tb.range_max_m   = kRangeMaxM;
    tb.beam_id       = ret.beam_id;
    bus_.update_trace(tb);

    const auto ship = bus_.ship();
    types::GeoPosition geo;
    geo.latitude_deg  = ship.latitude_deg;
    geo.longitude_deg = ship.longitude_deg;
    geo.altitude_m    = ship.altitude_m;

    // Peak picking above the CFAR threshold
    for (int i = 1; i < n - 1; ++i) {
        if (mag[i] > kCfarThreshold && mag[i] >= mag[i-1] && mag[i] > mag[i+1]) {
            const double range_m = (static_cast<double>(i) / n) * kRangeMaxM;
            const double snr_db  = 20.0 * std::log10(mag[i] / kNoiseSigma);

            types::DetectionEvent det;
            det.sensor_id     = 0; // constant key: one DDS instance
            det.detection_id  = detection_id_++;
            det.timestamp     = SimClock::stamp();
            det.ship_position = geo;
            det.range_m       = range_m;
            det.azimuth_deg   = ret.azimuth_deg;
            det.elevation_deg = ret.elevation_deg;
            det.amplitude     = mag[i];
            det.snr_db        = snr_db;
            det_writer_.write(det);
            // PPI blips reach the UI via HmiUi's DetectionEvent subscription.
        }
    }
}

} // namespace radar::app
