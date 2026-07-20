#include "TrackManager.hpp"

#include <algorithm>
#include <chrono>
#include <memory>

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
        auto samples = reader.take();
        for (const auto& s : samples)
            if (s.info().valid())
                (owner_->*Method)(s.data());
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

void TrackManager::on_detection(const types::DetectionEvent& det) {
    std::lock_guard lk(pending_mutex_);
    pending_.push_back(det); // batched; consumed at 10 Hz by update_loop
}

void TrackManager::update_loop() {
    using namespace std::chrono;
    auto next = steady_clock::now();

    while (!stop_.load()) {
        next += milliseconds(100); // 10 Hz track update

        if (bus_.reset_requested.exchange(false)) {
            // Dispose every live instance so subscribers (HMI-UI, Studio)
            // watch the tracks vanish instead of timing out.
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

        std::vector<CoreDetection> dets;
        dets.reserve(batch.size());
        for (const auto& d : batch)
            dets.push_back(CoreDetection{d.range_m, d.azimuth_deg, d.elevation_deg});

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

        for (const auto& t : core_.tracks()) {
            if (t.hits < 2) continue; // publish confirmed tracks only
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

            msg.covariance.resize(types::COVARIANCE_SIZE, 0.0);
            msg.covariance[0] = msg.covariance[4] = msg.covariance[8] = 2500.0;
            msg.classification =
                static_cast<types::TrackClassification>(t.classification);
            msg.quality = t.quality;
            writer_.write(msg);
        }

        std::this_thread::sleep_until(next);
    }
}

} // namespace radar::app
