#include "Beamformer.hpp"

#include <chrono>
#include <memory>

#include "Log.hpp"
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
    beam_id_.store(command.beam_id);
    commanded_azimuth_deg_.store(command.azimuth_deg);
}

void Beamformer::on_calibration_status(
        const types::CalibrationStatus& status) {
    rma_offline_mask_.store(
        static_cast<uint32_t>(status.rma_offline_mask) & 0xFFFFu);
}

void Beamformer::publish_loop() {
    using namespace std::chrono;

    auto next = steady_clock::now();
    uint32_t cached_rma_mask = 0xFFFFFFFFu;
    BeamPattern pattern;

    types::BeamPatternStatus status;
    status.array_id = 0;
    status.azimuth_pattern_db.resize(kBeamPatternSampleCount);

    while (!stop_.load()) {
        next += milliseconds(50); // 20 Hz

        const int64_t beam_id = beam_id_.load();
        if (beam_id < 0) {
            std::this_thread::sleep_until(next);
            continue;
        }

        const uint32_t rma_mask = rma_offline_mask_.load() & 0xFFFFu;
        if (rma_mask != cached_rma_mask) {
            pattern = BeamPatternModel::calculate(rma_mask);
            cached_rma_mask = rma_mask;
            RADAR_LOG << "[Beamformer] beam pattern mask="
                      << pattern.rma_offline_mask
                      << " active=" << pattern.active_elements
                      << " loss_db=" << pattern.gain_loss_db
                      << " bw_deg=" << pattern.beamwidth_3db_deg
                      << " psl_db=" << pattern.peak_sidelobe_level_db
                      << " error_deg=" << pattern.boresight_error_deg
                      << "\n";
        }

        status.timestamp = SimClock::stamp();
        status.beam_id = static_cast<int32_t>(beam_id);
        status.rma_offline_mask =
            static_cast<int32_t>(pattern.rma_offline_mask);
        status.commanded_azimuth_deg = commanded_azimuth_deg_.load();
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
        for (int i = 0; i < kBeamPatternSampleCount; ++i)
            status.azimuth_pattern_db[i] = pattern.azimuth_pattern_db[i];

        pattern_writer_.write(status);
        std::this_thread::sleep_until(next);
    }
}

} // namespace radar::app
