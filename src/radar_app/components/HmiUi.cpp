#include "HmiUi.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <memory>
#include <vector>

#include "Log.hpp"
#include "PeriodicDeadline.hpp"
#include "RadarFaces.hpp"
#include "SimClock.hpp"

namespace radar::app {

namespace {

// Listener adapter: forwards loaned batches to a member function. The
// member pointer is formed inside HmiUi::start() (member context), so the
// callbacks can stay ordinary public members.
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

// Track listener: also handles instance disposal so dropped/reset tracks
// disappear from the UI immediately instead of waiting for the age-out.
class TrackListener : public dds::sub::NoOpDataReaderListener<types::TargetTrack> {
public:
    explicit TrackListener(HmiUi* owner) : owner_(owner) {}
    void on_data_available(dds::sub::DataReader<types::TargetTrack>& reader) override {
        types::TargetTrack sample;
        dds::sub::SampleInfo info;
        for (int i = 0;
             i < 256 && reader.extensions().take(sample, info); ++i) {
            if (info.valid()) {
                owner_->on_track(sample, info.instance_handle());
                continue;
            }
            // Invalid sample on take(): instance lifecycle event (dispose or
            // no writers). The reader can reclaim its key lookup before a
            // remote-writer removal callback runs, so resolve the track from
            // the instance handle captured with valid samples instead of
            // calling key_value() here.
            owner_->on_track_dropped(info.instance_handle());
        }
    }
private:
    HmiUi* owner_;
};

} // namespace

void HmiUi::start() {
    auto track_topic = radds::make_topic<types::TargetTrack>(
        participant_, dds_names::TOPIC_TARGET_TRACK);
    auto det_topic = radds::make_topic<types::DetectionEvent>(
        participant_, dds_names::TOPIC_DETECTION_EVENT);
    auto ship_base_topic = radds::make_topic<types::ShipPosition>(
        participant_, dds_names::TOPIC_SHIP_POSITION);
    ship_topic_ = dds::topic::ContentFilteredTopic<types::ShipPosition>(
        ship_base_topic,
        "HmiOwnShip",
        dds::topic::Filter("source_id = 0"));
    auto cal_topic = radds::make_topic<types::CalibrationStatus>(
        participant_, dds_names::TOPIC_CALIBRATION_STATUS);
    auto pattern_topic = radds::make_topic<types::BeamPatternStatus>(
        participant_, dds_names::TOPIC_BEAM_PATTERN_STATUS);

    track_reader_ = radds::make_reader<types::TargetTrack>(
        subscriber_, track_topic, dds_names::PROFILE_TARGET_TRACK);
    det_reader_ = radds::make_reader<types::DetectionEvent>(
        subscriber_, det_topic, dds_names::PROFILE_DETECTION_EVENT);
    ship_reader_ = radds::make_reader<types::ShipPosition>(
        subscriber_, ship_topic_, dds_names::PROFILE_SHIP_POSITION);
    cal_reader_ = radds::make_reader<types::CalibrationStatus>(
        subscriber_, cal_topic, dds_names::PROFILE_CALIBRATION_STATUS);
    pattern_reader_ = radds::make_reader<types::BeamPatternStatus>(
        subscriber_, pattern_topic, dds_names::PROFILE_BEAM_PATTERN_STATUS);

    track_reader_.set_listener(std::make_shared<TrackListener>(this),
                               dds::core::status::StatusMask::data_available());
    det_reader_.set_listener(
        std::make_shared<ForwardingListener<types::DetectionEvent, HmiUi,
                                            &HmiUi::on_detection>>(this),
        dds::core::status::StatusMask::data_available());
    ship_reader_.set_listener(
        std::make_shared<ForwardingListener<types::ShipPosition, HmiUi,
                                            &HmiUi::on_ship>>(this),
        dds::core::status::StatusMask::data_available());
    cal_reader_.set_listener(
        std::make_shared<ForwardingListener<types::CalibrationStatus, HmiUi,
                                            &HmiUi::on_calibration>>(this),
        dds::core::status::StatusMask::data_available());
    pattern_reader_.set_listener(
        std::make_shared<ForwardingListener<types::BeamPatternStatus, HmiUi,
                                            &HmiUi::on_beam_pattern>>(this),
        dds::core::status::StatusMask::data_available());

    spawn([this] { housekeeping_loop(); });
}

void HmiUi::stop() {
    stop_.store(true);
    detach_listener(track_reader_);
    detach_listener(det_reader_);
    detach_listener(ship_reader_);
    detach_listener(cal_reader_);
    detach_listener(pattern_reader_);
    join_all();
}

// --- DDS callbacks (receive threads) ---------------------------------------

void HmiUi::on_track(
        const types::TargetTrack& t,
        const dds::core::InstanceHandle& instance_handle) {
    TrackView v;
    v.track_id       = t.track_id;
    v.x_m            = t.position.x_east_m;
    v.y_m            = t.position.y_north_m;
    v.z_m            = t.position.z_up_m;
    v.vx_mps         = t.velocity.x_east_m;
    v.vy_mps         = t.velocity.y_north_m;
    v.vz_mps         = t.velocity.z_up_m;
    v.classification = static_cast<int32_t>(t.classification);
    v.quality        = t.quality;
    v.sim_millis     = t.timestamp.sim_millis;

    std::lock_guard lk(tracks_mutex_);
    tracks_.insert_or_assign(
        v.track_id,
        TrackEntry{v, instance_handle, std::chrono::steady_clock::now()});
}

void HmiUi::on_track_dropped(
        const dds::core::InstanceHandle& instance_handle) {
    std::lock_guard lk(tracks_mutex_);
    for (auto it = tracks_.begin(); it != tracks_.end();) {
        if (it->second.instance_handle == instance_handle)
            it = tracks_.erase(it);
        else
            ++it;
    }
}

void HmiUi::on_detection(const types::DetectionEvent& d) {
    if (!faces::valid(d.sensor_id))
        return;
    bus_.detection_blips.push_overwrite(BlipView{
        d.sensor_id, d.range_m, d.azimuth_deg, d.elevation_deg,
        d.amplitude, d.snr_db, d.timestamp.sim_millis});
}

void HmiUi::on_ship(const types::ShipPosition& s) {
    if (s.source_id != 0) return; // defensive: CFT admits only own-ship INS
    bus_.update_ship_display(ShipView{
        s.latitude_deg, s.longitude_deg, s.altitude_m,
        s.heading_deg, s.course_deg, s.speed_mps,
        s.pitch_deg, s.roll_deg, s.timestamp.sim_millis});
}

void HmiUi::on_calibration(const types::CalibrationStatus& c) {
    if (!faces::valid(c.array_id))
        return;
    double drift_sum = 0.0;
    const int n = static_cast<int>(c.element_drift_db.size());
    for (int i = 0; i < n; ++i)
        drift_sum += std::fabs(static_cast<double>(c.element_drift_db[i]));

    bus_.update_health(HealthView{
        c.array_id,
        static_cast<int32_t>(c.overall_status),
        c.failed_element_count,
        n > 0 ? n : types::MAX_ARRAY_ELEMENTS,
        c.temperature_c,
        n > 0 ? drift_sum / n : 0.0,
        c.timestamp.sim_millis,
        static_cast<uint32_t>(c.rma_offline_mask)});

    // ARRAY FACE pane: full drift vector + RMA mask (1 Hz, cheap copy;
    // bounded_sequence is not std::vector, so copy element-wise).
    bus_.update_array_grid(
        c.array_id,
        std::vector<float>(c.element_drift_db.begin(), c.element_drift_db.end()),
        static_cast<uint32_t>(c.rma_offline_mask),
        c.timestamp.sim_millis);
}

void HmiUi::on_beam_pattern(const types::BeamPatternStatus& p) {
    if (!faces::valid(p.array_id))
        return;
    const uint32_t mask = static_cast<uint32_t>(p.rma_offline_mask);
    const auto face_index = static_cast<std::size_t>(p.array_id);
    if (last_pattern_masks_[face_index].exchange(mask) != mask) {
        RADAR_LOG << "[HmiUi] face="
                  << faces::kDefinitions[face_index].short_name
                  << " beam overlay mask=" << mask
                  << " loss_db=" << p.gain_loss_db
                  << " bw_deg=" << p.beamwidth_3db_deg
                  << " psl_db=" << p.peak_sidelobe_level_db
                  << " error_deg=" << p.boresight_error_deg
                  << "\n";
    }
    bus_.update_beam_pattern(BeamPatternView{
        p.array_id,
        p.beam_id,
        mask,
        p.commanded_azimuth_deg,
        p.boresight_error_deg,
        p.gain_loss_db,
        p.beamwidth_3db_deg,
        p.peak_sidelobe_level_db,
        p.left_sidelobe_offset_deg,
        p.right_sidelobe_offset_deg,
        p.pattern_start_offset_deg,
        p.pattern_step_deg,
        std::vector<float>(p.azimuth_pattern_db.begin(),
                           p.azimuth_pattern_db.end()),
        p.timestamp.sim_millis});
}

// --- view publisher / age-out backstop -------------------------------------

void HmiUi::housekeeping_loop() {
    using namespace std::chrono;
    auto next = steady_clock::now();
    int hb_cycles = 0; // heartbeat diagnostics: 2 s cadence, see TrackManager
    while (!stop_.load()) {
        next = advance_periodic_deadline(
            next, milliseconds(200)); // 5 Hz view refresh

        const auto now = steady_clock::now();
        std::vector<TrackView> views;
        std::size_t map_size = 0;
        {
            std::lock_guard lk(tracks_mutex_);
            map_size = tracks_.size();
            for (auto it = tracks_.begin(); it != tracks_.end();) {
                // Source sim_millis is process-relative and cannot be used
                // for age comparisons when another radar publisher joins
                // the DDS domain. Age from local reception time instead.
                if (now - it->second.received_at > kTrackStale)
                    it = tracks_.erase(it); // dispose missed; age out
                else {
                    views.push_back(it->second.view);
                    ++it;
                }
            }
        }
        bus_.update_tracks(views);

        if (++hb_cycles >= 10) { // 2 s at 5 Hz
            hb_cycles = 0;
            RADAR_LOG << "[HmiUi] hb tracks=" << map_size
                      << " views=" << views.size() << std::endl;
        }

        std::this_thread::sleep_until(next);
    }
}

} // namespace radar::app
