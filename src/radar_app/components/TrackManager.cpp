#include "TrackManager.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>

#include "EffectiveRangeModel.hpp"
#include "Log.hpp"
#include "FaceDetectionFusion.hpp"
#include "PeriodicDeadline.hpp"
#include "RadarFaces.hpp"
#include "SimClock.hpp"

namespace radar::app {

namespace {

// Listener adapter: forwards loaned batches to a member function. The
// member pointer is formed inside TrackManager::start() (member context),
// so on_detection can stay private.
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

void TrackManager::start() {
    auto det_topic   = radds::make_topic<types::DetectionEvent>(participant_, dds_names::TOPIC_DETECTION_EVENT);
    auto track_topic = radds::make_topic<types::TargetTrack>(participant_, dds_names::TOPIC_TARGET_TRACK);

    reader_ = radds::make_reader<types::DetectionEvent>(subscriber_, det_topic, dds_names::PROFILE_DETECTION_EVENT);
    writer_ = radds::make_writer<types::TargetTrack>(publisher_, track_topic, dds_names::PROFILE_TARGET_TRACK);

    reader_.set_listener(
        std::make_shared<ForwardingListener<types::DetectionEvent, TrackManager,
                                            &TrackManager::on_detection>>(this),
        dds::core::status::StatusMask::data_available());

    spawn([this] { update_loop(); });
}

void TrackManager::stop() {
    stop_.store(true);
    detach_listener(reader_);
    join_all();

    // Dispose every keyed instance while the writer is still alive. Remote
    // HMIs can then remove this publisher's rows immediately instead of
    // relying on a no-writers notification or the stale-reception backstop.
    if (writer_ != dds::core::null && bus_.dispose_enabled.load()) {
        for (const auto& [id, handle] : handles_) {
            (void)id;
            writer_.dispose_instance(handle);
        }
    }
    handles_.clear();
    core_.reset();
}

void TrackManager::on_detection(const types::DetectionEvent& det) {
    if (!faces::valid(det.sensor_id))
        return;
    std::lock_guard lk(pending_mutex_);
    pending_.push_back(det); // batched; consumed at 10 Hz by update_loop
}

void TrackManager::update_loop() {
    using namespace std::chrono;
    auto next = steady_clock::now();

    // Heartbeat diagnostics (2026-07-20): localizes empty-track-table
    // reports. Prints every 2 s; if the line stops while the app keeps
    // rendering, this thread wedged — that ties the empty table to the
    // windowed-crash corruptor (victim #8 died inside writer_.write here).
    uint64_t dets_in = 0;
    int hb_cycles = 0;

    while (!stop_.load()) {
        next = advance_periodic_deadline(
            next, milliseconds(100)); // 10 Hz track update

        if (bus_.reset_requested.exchange(false)) {
            // Dispose every live instance so subscribers (HMI-UI, Studio)
            // watch the tracks vanish instead of timing out.
            RADAR_LOG << "[TrackManager] reset consumed — tracks cleared" << std::endl;
            if (bus_.dispose_enabled.load())
                for (auto& [id, h] : handles_) writer_.dispose_instance(h);
            handles_.clear();
            core_.reset();
        }

        std::vector<types::DetectionEvent> batch;
        {
            std::lock_guard lk(pending_mutex_);
            batch.swap(pending_);
        }
        dets_in += batch.size();

        std::vector<FaceDetection> face_detections;
        face_detections.reserve(batch.size());
        for (const auto& d : batch) {
            face_detections.push_back(FaceDetection{
                d.sensor_id, d.timestamp.sim_millis,
                d.range_m, d.azimuth_deg, d.elevation_deg, d.snr_db});
        }
        const auto fused_detections =
            fuse_resolution_cell_detections(face_detections);

        std::vector<CoreDetection> dets;
        dets.reserve(fused_detections.size());
        for (const auto& d : fused_detections) {
            dets.push_back(
                CoreDetection{
                    d.range_m, d.azimuth_deg, d.elevation_deg,
                    d.snr_db});
        }

        const auto ship = bus_.ship();
        const int64_t now_ms = SimClock::sim_millis();

        // TrackerCore does association/filter/coast; we dispose the dropped.
        const auto dropped = core_.update(dets, ship.heading_deg, now_ms);
        for (const int64_t id : dropped) {
            auto h = handles_.find(id);
            if (h != handles_.end()) {
                if (bus_.dispose_enabled.load())
                    writer_.dispose_instance(h->second);
                handles_.erase(h);
            }
        }

        int published = 0;
        for (const auto& t : core_.tracks()) {
            // A confirmed track has three independent scan visits inside the
            // five-volume initiation window. Multiple pulse or adjacent-beam
            // plots from one illumination can never satisfy this gate.
            if (!t.confirmed) continue;
            ++published;
            auto hit = handles_.find(t.id);
            if (hit == handles_.end()) {
                types::TargetTrack reg;
                reg.track_id = t.id;
                hit = handles_.emplace(t.id, writer_.register_instance(reg)).first;
            }

            types::TargetTrack msg;
            msg.track_id  = t.id;
            msg.timestamp = SimClock::stamp();

            msg.position.x_east_m  = t.x;
            msg.position.y_north_m = t.y;
            msg.position.z_up_m    = t.z;
            msg.velocity.x_east_m  = t.vx;
            msg.velocity.y_north_m = t.vy;
            msg.velocity.z_up_m    = t.vz;
            msg.acceleration.x_east_m  = 0.0;
            msg.acceleration.y_north_m = 0.0;
            msg.acceleration.z_up_m    = 0.0;

            const double slant_range_m =
                std::hypot(std::hypot(t.x, t.y), t.z);
            const double azimuth_world_deg =
                std::atan2(t.x, t.y)
                * 180.0 / effective_range::kPi;
            const double elevation_deg =
                std::atan2(t.z, std::hypot(t.x, t.y))
                * 180.0 / effective_range::kPi;
            const auto covariance =
                effective_range::cartesian_position_covariance(
                    slant_range_m,
                    azimuth_world_deg,
                    elevation_deg,
                    effective_range::MeasurementUncertainty{
                        t.range_stddev_m,
                        t.azimuth_stddev_deg,
                        t.elevation_stddev_deg});
            msg.covariance.resize(types::COVARIANCE_SIZE, 0.0);
            for (std::size_t i = 0; i < covariance.size(); ++i)
                msg.covariance[i] = covariance[i];
            msg.classification =
                static_cast<types::TrackClassification>(t.classification);
            msg.quality = t.quality;
            writer_.write(msg);
        }

        if (++hb_cycles >= 20) { // 2 s at 10 Hz
            hb_cycles = 0;
            RADAR_LOG << "[TrackManager] hb dets_in=" << dets_in
                      << " alive=" << core_.tracks().size()
                      << " published=" << published
                      << " handles=" << handles_.size() << std::endl;
        }

        std::this_thread::sleep_until(next);
    }
}

} // namespace radar::app
