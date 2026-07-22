#include "HmiUi.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <memory>
#include <vector>

#include "Log.hpp"
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
                owner_->on_track(sample);
                continue;
            }
            // Invalid sample on take(): instance lifecycle event (dispose /
            // unregister). Recover the key to drop the track from the UI.
            // (RTI 7.7: SampleInfo has no instance_state(); key_value uses
            // the two-argument out-param form.)
            try {
                types::TargetTrack key;
                reader.key_value(key, info.instance_handle());
                owner_->on_track_dropped(key.track_id);
            } catch (const dds::core::Error&) {
                // instance already reclaimed; the age-out will catch it
            }
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
    auto ship_topic = radds::make_topic<types::ShipPosition>(
        participant_, dds_names::TOPIC_SHIP_POSITION);
    auto cal_topic = radds::make_topic<types::CalibrationStatus>(
        participant_, dds_names::TOPIC_CALIBRATION_STATUS);

    track_reader_ = radds::make_reader<types::TargetTrack>(
        subscriber_, track_topic, dds_names::PROFILE_TARGET_TRACK);
    det_reader_ = radds::make_reader<types::DetectionEvent>(
        subscriber_, det_topic, dds_names::PROFILE_DETECTION_EVENT);
    ship_reader_ = radds::make_reader<types::ShipPosition>(
        subscriber_, ship_topic, dds_names::PROFILE_SHIP_POSITION);
    cal_reader_ = radds::make_reader<types::CalibrationStatus>(
        subscriber_, cal_topic, dds_names::PROFILE_CALIBRATION_STATUS);

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

    spawn([this] { housekeeping_loop(); });
}

void HmiUi::stop() {
    stop_.store(true);
    detach_listener(track_reader_);
    detach_listener(det_reader_);
    detach_listener(ship_reader_);
    detach_listener(cal_reader_);
    join_all();
}

// --- DDS callbacks (receive threads) ---------------------------------------

void HmiUi::on_track(const types::TargetTrack& t) {
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
    tracks_[v.track_id] = v;
}

void HmiUi::on_track_dropped(int64_t track_id) {
    std::lock_guard lk(tracks_mutex_);
    tracks_.erase(track_id);
}

void HmiUi::on_detection(const types::DetectionEvent& d) {
    bus_.detection_blips.push_overwrite(BlipView{
        d.range_m, d.azimuth_deg, d.elevation_deg,
        d.amplitude, d.snr_db, d.timestamp.sim_millis});
}

void HmiUi::on_ship(const types::ShipPosition& s) {
    if (s.source_id != 0) return; // panel shows own-ship INS (key 0)
    bus_.update_ship_display(ShipView{
        s.latitude_deg, s.longitude_deg, s.altitude_m,
        s.heading_deg, s.course_deg, s.speed_mps,
        s.pitch_deg, s.roll_deg, s.timestamp.sim_millis});
}

void HmiUi::on_calibration(const types::CalibrationStatus& c) {
    double drift_sum = 0.0;
    const int n = static_cast<int>(c.element_drift_db.size());
    for (int i = 0; i < n; ++i)
        drift_sum += std::fabs(static_cast<double>(c.element_drift_db[i]));

    bus_.update_health(HealthView{
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
        std::vector<float>(c.element_drift_db.begin(), c.element_drift_db.end()),
        static_cast<uint32_t>(c.rma_offline_mask),
        c.timestamp.sim_millis);
}

// --- view publisher / age-out backstop -------------------------------------

void HmiUi::housekeeping_loop() {
    using namespace std::chrono;
    auto next = steady_clock::now();
    int hb_cycles = 0; // heartbeat diagnostics: 2 s cadence, see TrackManager
    while (!stop_.load()) {
        next += milliseconds(200); // 5 Hz view refresh

        const int64_t now_ms = SimClock::sim_millis();
        std::vector<TrackView> views;
        std::size_t map_size = 0;
        {
            std::lock_guard lk(tracks_mutex_);
            map_size = tracks_.size();
            for (auto it = tracks_.begin(); it != tracks_.end();) {
                if (now_ms - it->second.sim_millis > kTrackStaleMs)
                    it = tracks_.erase(it); // dispose missed; age out
                else {
                    views.push_back(it->second);
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
