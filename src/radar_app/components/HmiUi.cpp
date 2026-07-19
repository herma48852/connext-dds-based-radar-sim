#include "HmiUi.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <vector>

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
        auto samples = reader.take();
        for (const auto& s : samples)
            if (s.info().valid())
                (owner_->*Method)(s.data());
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
        auto samples = reader.take();
        for (const auto& s : samples) {
            if (s.info().valid()) {
                owner_->on_track(s.data());
                continue;
            }
            // Invalid sample: instance state change (dispose / no writers).
            const auto st = s.info().instance_state();
            if (st == dds::sub::status::InstanceState::not_alive_disposed() ||
                st == dds::sub::status::InstanceState::not_alive_no_writers()) {
                try {
                    const auto key = reader.key_value(s.info().instance_handle());
                    owner_->on_track_dropped(key.track_id);
                } catch (const dds::core::Error&) {
                    // instance already reclaimed; the age-out will catch it
                }
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
        c.timestamp.sim_millis});
}

// --- view publisher / age-out backstop -------------------------------------

void HmiUi::housekeeping_loop() {
    using namespace std::chrono;
    auto next = steady_clock::now();
    while (!stop_.load()) {
        next += milliseconds(200); // 5 Hz view refresh

        const int64_t now_ms = SimClock::sim_millis();
        std::vector<TrackView> views;
        {
            std::lock_guard lk(tracks_mutex_);
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

        std::this_thread::sleep_until(next);
    }
}

} // namespace radar::app
