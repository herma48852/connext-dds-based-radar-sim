#include "Beamformer.hpp"

#include <chrono>
#include <memory>

#include "Log.hpp"
#include "PeriodicDeadline.hpp"
#include "RadarFaces.hpp"
#include "RadarRfModel.hpp"
#include "SimClock.hpp"

namespace radar::app {

namespace {
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

void Beamformer::start() {
    auto beam_topic = radds::make_topic<types::BeamCommand>(
        participant_, dds_names::TOPIC_BEAM_COMMAND);
    auto calibration_topic = radds::make_topic<types::CalibrationStatus>(
        participant_, dds_names::TOPIC_CALIBRATION_STATUS);
    auto pattern_topic = radds::make_topic<types::BeamPatternStatus>(
        participant_, dds_names::TOPIC_BEAM_PATTERN_STATUS);

    pattern_writer_ = radds::make_writer<types::BeamPatternStatus>(
        publisher_, pattern_topic, dds_names::PROFILE_BEAM_PATTERN_STATUS);
    beam_reader_ = radds::make_reader<types::BeamCommand>(
        subscriber_, beam_topic, dds_names::PROFILE_BEAM_COMMAND);
    calibration_reader_ = radds::make_reader<types::CalibrationStatus>(
        subscriber_, calibration_topic,
        dds_names::PROFILE_CALIBRATION_STATUS);

    beam_reader_.set_listener(
        std::make_shared<ForwardingListener<types::BeamCommand, Beamformer,
                                            &Beamformer::on_beam_command>>(this),
        dds::core::status::StatusMask::data_available());
    calibration_reader_.set_listener(
        std::make_shared<
            ForwardingListener<types::CalibrationStatus, Beamformer,
                               &Beamformer::on_calibration_status>>(this),
        dds::core::status::StatusMask::data_available());

    spawn([this] { publish_loop(); });
}

void Beamformer::stop() {
    stop_.store(true);
    detach_listener(beam_reader_);
    detach_listener(calibration_reader_);
    join_all();
}

void Beamformer::on_beam_command(const types::BeamCommand& command) {
    if (!faces::valid(command.scheduler_id))
        return;
    const auto i = static_cast<std::size_t>(command.scheduler_id);
    beam_ids_[i].store(command.beam_id);
    commanded_azimuth_deg_[i].store(command.azimuth_deg);
}

void Beamformer::on_calibration_status(
        const types::CalibrationStatus& status) {
    if (!faces::valid(status.array_id))
        return;
    rma_offline_masks_[static_cast<std::size_t>(status.array_id)].store(
        static_cast<uint32_t>(status.rma_offline_mask) & 0xFFFFu);
}

void Beamformer::publish_loop() {
    using namespace std::chrono;

    auto next = steady_clock::now();
    std::array<uint32_t, faces::kFaceCount> cached_rma_masks{};
    cached_rma_masks.fill(0xFFFFFFFFu);
    std::array<BeamPattern, faces::kFaceCount> patterns{};

    types::BeamPatternStatus status;
    status.azimuth_pattern_db.resize(kBeamPatternSampleCount);
    status.carrier_frequency_hz = rf_model::kCarrierFrequencyHz;
    status.wavelength_m = rf_model::kWavelengthM;
    status.element_pitch_m = rf_model::kElementPitchM;
    status.waveform_bandwidth_hz = rf_model::kWaveformBandwidthHz;
    status.pulse_repetition_frequency_hz =
        rf_model::kPulseRepetitionFrequencyHz;
    status.pulse_width_sec = rf_model::kPulseWidthSec;
    status.range_resolution_m = rf_model::kRangeResolutionM;
    status.unambiguous_range_m = rf_model::kUnambiguousRangeM;

    while (!stop_.load()) {
        next = advance_periodic_deadline(
            next, milliseconds(50)); // 20 Hz

        for (const auto& face : faces::kDefinitions) {
            const auto i = static_cast<std::size_t>(face.id);
            const int64_t beam_id = beam_ids_[i].load();
            if (beam_id < 0)
                continue;

            const uint32_t rma_mask =
                rma_offline_masks_[i].load() & 0xFFFFu;
            if (rma_mask != cached_rma_masks[i]) {
                patterns[i] = BeamPatternModel::calculate(rma_mask);
                cached_rma_masks[i] = rma_mask;
                RADAR_LOG << "[Beamformer] face=" << face.short_name
                          << " beam pattern mask="
                          << patterns[i].rma_offline_mask
                          << " active=" << patterns[i].active_elements
                          << " loss_db=" << patterns[i].gain_loss_db
                          << " bw_deg=" << patterns[i].beamwidth_3db_deg
                          << " psl_db="
                          << patterns[i].peak_sidelobe_level_db
                          << " error_deg="
                          << patterns[i].boresight_error_deg
                          << "\n";
            }
            const auto& pattern = patterns[i];
            status.array_id = face.id;
            status.timestamp = SimClock::stamp();
            status.beam_id = static_cast<int32_t>(beam_id);
            status.rma_offline_mask =
                static_cast<int32_t>(pattern.rma_offline_mask);
            status.commanded_azimuth_deg =
                commanded_azimuth_deg_[i].load();
            status.boresight_error_deg = pattern.boresight_error_deg;
            status.gain_loss_db = pattern.gain_loss_db;
            status.beamwidth_3db_deg = pattern.beamwidth_3db_deg;
            status.peak_sidelobe_level_db =
                pattern.peak_sidelobe_level_db;
            status.left_sidelobe_offset_deg =
                pattern.left_sidelobe_offset_deg;
            status.right_sidelobe_offset_deg =
                pattern.right_sidelobe_offset_deg;
            status.pattern_start_offset_deg = kBeamPatternStartDeg;
            status.pattern_step_deg = kBeamPatternStepDeg;
            for (int sample_index = 0;
                 sample_index < kBeamPatternSampleCount; ++sample_index) {
                status.azimuth_pattern_db[sample_index] =
                    pattern.azimuth_pattern_db[sample_index];
            }
            pattern_writer_.write(status);
        }
        std::this_thread::sleep_until(next);
    }
}

} // namespace radar::app
