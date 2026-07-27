#include "DetectionProcessor.hpp"
#include "DetectionSignalProcessing.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <memory>

#include "Log.hpp"
#include "PeriodicDeadline.hpp"
#include "RadarFaces.hpp"
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
    beam_reader_  = radds::make_reader<types::BeamCommand>(subscriber_, beam_topic, dds_names::PROFILE_BEAM_COMMAND);
    pattern_reader_ = radds::make_reader<types::BeamPatternStatus>(
        subscriber_, pattern_topic, dds_names::PROFILE_BEAM_PATTERN_STATUS);
    raw_reader_   = radds::make_reader<types::RawReturn>(subscriber_, raw_topic, dds_names::PROFILE_RAW_RETURN);
    truth_reader_ = radds::make_reader<types::TargetTruth>(subscriber_, truth_topic, dds_names::PROFILE_TARGET_TRUTH);

    // Listeners for the high-rate paths (BeamCommand, RawReturn, TargetTruth).
    beam_reader_.set_listener(
        std::make_shared<ForwardingListener<types::BeamCommand, DetectionProcessor,
                                            &DetectionProcessor::on_beam_command>>(this),
        dds::core::status::StatusMask::data_available());
    pattern_reader_.set_listener(
        std::make_shared<
            ForwardingListener<types::BeamPatternStatus, DetectionProcessor,
                               &DetectionProcessor::on_beam_pattern>>(this),
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
    detach_listener(pattern_reader_);
    detach_listener(raw_reader_);
    detach_listener(truth_reader_);
    join_all();
    std::lock_guard lock(dwell_power_mutex_);
    for (auto& accumulator : dwell_power_accumulators_)
        accumulator.clear();
}

void DetectionProcessor::on_beam_command(const types::BeamCommand& cmd) {
    if (!faces::valid(cmd.scheduler_id))
        return;
    const auto i = static_cast<std::size_t>(cmd.scheduler_id);
    dwell_beam_ids_[i].store(cmd.beam_id);
    dwell_az_deg_[i].store(cmd.azimuth_deg);
    dwell_el_deg_[i].store(cmd.elevation_deg);
}

void DetectionProcessor::on_beam_pattern(
        const types::BeamPatternStatus& status) {
    if (!faces::valid(status.array_id))
        return;
    const auto face_index = static_cast<std::size_t>(status.array_id);
    BeamPattern pattern;
    pattern.rma_offline_mask =
        static_cast<uint32_t>(status.rma_offline_mask) & 0xFFFFu;
    pattern.active_elements =
        1024 - 64 * std::popcount(pattern.rma_offline_mask);
    pattern.active_fraction =
        static_cast<double>(pattern.active_elements) / 1024.0;
    pattern.gain_loss_db = status.gain_loss_db;
    pattern.boresight_error_deg = status.boresight_error_deg;
    pattern.beamwidth_3db_deg = status.beamwidth_3db_deg;
    pattern.peak_sidelobe_level_db = status.peak_sidelobe_level_db;
    pattern.left_sidelobe_offset_deg = status.left_sidelobe_offset_deg;
    pattern.right_sidelobe_offset_deg = status.right_sidelobe_offset_deg;
    pattern.azimuth_pattern_db.fill(-80.0f);
    const int sample_count = std::min<int>(
        kBeamPatternSampleCount,
        static_cast<int>(status.azimuth_pattern_db.size()));
    for (int i = 0; i < sample_count; ++i)
        pattern.azimuth_pattern_db[i] = status.azimuth_pattern_db[i];

    {
        std::lock_guard lk(pattern_mutex_);
        patterns_[face_index] = pattern;
    }
    receive_aperture_online_[face_index].store(
        receive_aperture_online(pattern.active_elements),
        std::memory_order_release);
    pattern_revisions_[face_index].fetch_add(
        1, std::memory_order_release);
}

void DetectionProcessor::on_truth(const types::TargetTruth& t) {
    std::lock_guard lk(truth_mutex_);
    auto& s = truth_[t.target_id];
    s.x  = t.position.x_east_m;  s.y  = t.position.y_north_m;  s.z  = t.position.z_up_m;
    s.vx = t.velocity.x_east_m;  s.vy = t.velocity.y_north_m;  s.vz = t.velocity.z_up_m;
    s.rcs_dbsm    = t.rcs_dbsm;
    s.target_type = static_cast<int32_t>(t.target_type);
    s.received_at = std::chrono::steady_clock::now();
}

// --- Receiver simulation: PRF-rate RawReturn synthesis for current dwell ---
void DetectionProcessor::return_synthesis_loop() {
    using namespace std::chrono;
    auto next = steady_clock::now();
    constexpr duration<double> kPulseRepetitionPeriod{
        1.0 / rf_model::kPulseRepetitionFrequencyHz};

    types::RawReturn sample;
    sample.range_bin_count = kRangeBins;
    sample.iq_samples.resize(2 * kRangeBins);

    // Use a physical nominal pattern during the brief DDS discovery interval
    // before each face's first BeamPatternStatus sample arrives.
    std::array<BeamPattern, faces::kFaceCount> local_patterns{};
    for (auto& pattern : local_patterns)
        pattern = BeamPatternModel::calculate(0u);
    std::array<uint64_t, faces::kFaceCount> observed_pattern_revisions{};
    std::array<uint32_t, faces::kFaceCount> logged_rma_masks{};
    logged_rma_masks.fill(0xFFFFFFFFu);
    std::vector<TruthState> truth_snapshot;

    while (!stop_.load()) {
        next = advance_periodic_deadline(
            next, kPulseRepetitionPeriod);

        const auto now = steady_clock::now();
        truth_snapshot.clear();
        {
            std::lock_guard lk(truth_mutex_);
            detection_model::prune_stale_truth(truth_, now);
            truth_snapshot.reserve(truth_.size());
            for (const auto& [id, target] : truth_) {
                (void)id;
                truth_snapshot.push_back(target);
            }
        }
        const double heading = bus_.ship().heading_deg;

        for (const auto& face : faces::kDefinitions) {
            const auto face_index = static_cast<std::size_t>(face.id);
            const int64_t beam_id = dwell_beam_ids_[face_index].load();
            if (beam_id < 0)
                continue;
            const double az_deg = dwell_az_deg_[face_index].load();
            const double el_deg = dwell_el_deg_[face_index].load();

            const uint64_t pattern_revision =
                pattern_revisions_[face_index].load(
                    std::memory_order_acquire);
            if (pattern_revision
                != observed_pattern_revisions[face_index]) {
                {
                    std::lock_guard lk(pattern_mutex_);
                    local_patterns[face_index] = patterns_[face_index];
                }
                observed_pattern_revisions[face_index] = pattern_revision;
            }
            const BeamPattern& pattern = local_patterns[face_index];

            // RMA-offline effect (Tier-1 physics): array gain ~ N_active,
            // azimuth beamwidth ~ 1/sqrt(N_active). A dark aperture retains
            // thermal noise on its I/Q stream but produces no detections.
            const uint32_t rma_mask = pattern.rma_offline_mask;
            if (observed_pattern_revisions[face_index] != 0 &&
                rma_mask != logged_rma_masks[face_index]) {
                logged_rma_masks[face_index] = rma_mask;
                RADAR_LOG
                    << "[DetectionProcessor] face=" << face.short_name
                    << " applied beam pattern mask="
                    << pattern.rma_offline_mask
                    << " active=" << pattern.active_elements
                    << " loss_db=" << pattern.gain_loss_db
                    << " bw_deg=" << pattern.beamwidth_3db_deg
                    << " psl_db=" << pattern.peak_sidelobe_level_db
                    << " error_deg=" << pattern.boresight_error_deg
                    << "\n";
            }
            const bool aperture_online =
                receive_aperture_online(pattern.active_elements);
            const double active = aperture_online
                ? pattern.active_fraction : 0.0;

            for (int i = 0; i < 2 * kRangeBins; ++i)
                sample.iq_samples[i] = noise_(rng_);

            if (aperture_online) {
                for (const auto& t : truth_snapshot) {
                    // Extrapolate the latest 50 Hz truth sample to this
                    // pulse. This creates coherent pulse-to-pulse carrier
                    // phase and therefore preserves Doppler information in
                    // the raw I/Q.
                    const double age_sec =
                        duration<double>(now - t.received_at).count();
                    const double x = t.x + t.vx * age_sec;
                    const double y = t.y + t.vy * age_sec;
                    const double z = t.z + t.vz * age_sec;
                    const double range_xy = std::hypot(x, y);
                    const double range = std::sqrt(x*x + y*y + z*z);
                    if (!detection_model::within_instrumented_range(range))
                        continue;

                    // world ENU azimuth -> ship-relative azimuth
                    const double az_world = std::atan2(x, y) / kDeg2Rad;
                    const double az_ship  = wrap180(az_world - heading);
                    const double beam_offset = wrap180(az_ship - az_deg);

                    // Use the calculated array factor for nominal and
                    // degraded beams alike. The physical ~3.2-degree nominal
                    // HPBW is wider than the 2.25-degree raster spacing, so
                    // adjacent half-power footprints overlap instead of
                    // leaving hard dead stripes.
                    const double main_offset =
                        wrap180(beam_offset - pattern.boresight_error_deg);
                    const double main_half_width =
                        pattern.beamwidth_3db_deg * 0.5;
                    double pattern_response = 0.0;
                    if (std::fabs(main_offset) <= main_half_width) {
                        pattern_response =
                            pattern.relative_amplitude(beam_offset);
                    } else if (rma_mask != 0) {
                        // Bound outage-created sidelobe theatre to the two
                        // dominant lobes. The extra -9 dB scale keeps
                        // ordinary air targets quiet while allowing a strong
                        // ship-sized return to make an occasional displaced
                        // ghost.
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
                    } else {
                        continue;
                    }

                    const double el_t =
                        std::atan2(z, range_xy) / kDeg2Rad;
                    // Elevation acceptance gates tile exactly without
                    // overlap. They are a classification partition, not the
                    // physical elevation array-factor HPBW.
                    if (std::fabs(el_t - el_deg) > 5.5)
                        continue;

                    const double amp =
                        detection_model::target_voltage_amplitude(
                            t.rcs_dbsm, range, active, pattern_response);
                    const double carrier_phase =
                        detection_model::two_way_carrier_phase_rad(range);
                    const double in_phase = std::cos(carrier_phase);
                    const double quadrature = std::sin(carrier_phase);

                    const int b0 = detection_model::range_bin_for(range);
                    for (int db = -1; db <= 1; ++db) {
                        const int b = b0 + db;
                        if (b < 0 || b >= kRangeBins) continue;
                        const double w =
                            detection_model::compressed_pulse_weight(db);
                        sample.iq_samples[2*b] +=
                            static_cast<float>(amp * w * in_phase);
                        sample.iq_samples[2*b+1] +=
                            static_cast<float>(amp * w * quadrature);
                    }
                }
            }

            sample.array_id = face.id;
            sample.beam_id = beam_id;
            sample.timestamp = SimClock::stamp();
            sample.azimuth_deg = az_deg;
            sample.elevation_deg = el_deg;
            raw_writer_.write(sample);
        }

        std::this_thread::sleep_until(next);
    }
}

// --- Signal processor: dwell integration + plot extraction ----------------
void DetectionProcessor::on_raw_return(const types::RawReturn& ret) {
    if (!faces::valid(ret.array_id))
        return;
    // Bound by the ACTUAL sequence length, not just the declared bin count:
    // a foreign/malformed publisher could send range_bin_count = N with a
    // shorter iq_samples buffer, and indexing would run off the end.
    const int n = std::min<int>(
        std::min<int>(ret.range_bin_count, kRangeBins),
        static_cast<int>(ret.iq_samples.size()) / 2);
    if (n < 3) return;

    // DDS may dispatch keyed samples from multiple receive threads. Serialize
    // the four small per-face accumulators and the shared DetectionEvent
    // writer/detection-id sequence.
    std::lock_guard lock(dwell_power_mutex_);
    const auto face_index = static_cast<std::size_t>(ret.array_id);
    auto& accumulator = dwell_power_accumulators_[face_index];

    // beam_id is the dwell boundary. Complete the previous 10-pulse power
    // average before accepting the first pulse of the new pointing.
    if (accumulator.active() && !accumulator.matches(ret.beam_id))
        publish_completed_dwell(face_index, accumulator);

    if (!accumulator.active()) {
        accumulator.begin(
            ret.beam_id, ret.azimuth_deg, ret.elevation_deg, n);
    }
    accumulator.accumulate(std::span<const float>(
        ret.iq_samples.data(), static_cast<std::size_t>(2 * n)));
}

void DetectionProcessor::publish_completed_dwell(
        std::size_t face_index,
        DwellPowerAccumulator& accumulator) {
    const int64_t beam_id = accumulator.beam_id();
    const double azimuth_deg = accumulator.azimuth_deg();
    const double elevation_deg = accumulator.elevation_deg();
    auto& magnitude = integrated_magnitudes_[face_index];
    if (!accumulator.complete(magnitude))
        return;

    TraceBuffer tb;
    tb.face_id       = static_cast<int32_t>(face_index);
    tb.magnitude     = magnitude;
    tb.azimuth_deg   = azimuth_deg;
    tb.elevation_deg = elevation_deg;
    tb.range_max_m   = kRangeMaxM;
    tb.beam_id       = beam_id;
    bus_.update_trace(tb);

    const auto ship = bus_.ship();
    types::GeoPosition geo;
    geo.latitude_deg  = ship.latitude_deg;
    geo.longitude_deg = ship.longitude_deg;
    geo.altitude_m    = ship.altitude_m;

    // Peak picking happens once per integrated dwell, not once per PRF
    // sample. Preserve the integrated thermal-noise A-scope for an offline
    // face, but do not let a dark aperture seed tracker plots.
    for_each_cfar_detection(
        magnitude,
        receive_aperture_online_[face_index].load(
                std::memory_order_acquire),
        static_cast<float>(kCfarThreshold),
        [this, face_index, azimuth_deg, elevation_deg,
         &geo](int i, float amplitude) {
            const double range_m =
                detection_model::range_m_for_bin(i);
            const double snr_db =
                20.0 * std::log10(
                    amplitude
                    / detection_model::kNoiseMagnitudeRms);

            types::DetectionEvent det;
            det.sensor_id     = static_cast<int32_t>(face_index);
            det.detection_id  = detection_id_++;
            det.timestamp     = SimClock::stamp();
            det.ship_position = geo;
            det.range_m       = range_m;
            det.azimuth_deg   = azimuth_deg;
            det.elevation_deg = elevation_deg;
            det.amplitude     = amplitude;
            det.snr_db        = snr_db;
            det_writer_.write(det);
            // PPI blips reach the UI via HmiUi's DetectionEvent subscription.
        });
}

} // namespace radar::app
