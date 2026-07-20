#include "TrackManager.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>

#include "SimClock.hpp"

namespace radar::app {

namespace {
constexpr double kDeg2Rad = 3.14159265358979323846 / 180.0;

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

// ship-relative polar -> ship-relative ENU (az 0 = bow, CW positive;
// ENU axes are north/east aligned, so rotate by ship heading)
void polar_to_enu(double range_m, double az_ship_deg, double el_deg,
                  double heading_deg, double& x, double& y, double& z) {
    const double az_world = (az_ship_deg + heading_deg) * kDeg2Rad;
    const double el       = el_deg * kDeg2Rad;
    const double rxy      = range_m * std::cos(el);
    x = rxy * std::sin(az_world);
    y = rxy * std::cos(az_world);
    z = range_m * std::sin(el);
}

int classify(double speed, double z) {
    using TC = types::TrackClassification;
    if (speed > 300.0)            return static_cast<int>(TC::CLASS_BALLISTIC);
    if (speed > 100.0)            return static_cast<int>(TC::CLASS_AIR_BREATHING);
    if (speed < 30.0 && z < 50.0) return static_cast<int>(TC::CLASS_SURFACE);
    return static_cast<int>(TC::CLASS_UNKNOWN);
}
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
    std::vector<Track> tracks;
    auto next = steady_clock::now();

    while (!stop_.load()) {
        next += milliseconds(100); // 10 Hz track update

        if (bus_.reset_requested.exchange(false)) {
            // Dispose every live instance so subscribers (HMI-UI, Studio)
            // watch the tracks vanish instead of timing out.
            if (bus_.dispose_enabled.load())
                for (auto& [id, h] : handles_) writer_.dispose_instance(h);
            handles_.clear();
            tracks.clear();
        }

        std::vector<types::DetectionEvent> batch;
        {
            std::lock_guard lk(pending_mutex_);
            batch.swap(pending_);
        }

        const auto ship = bus_.ship();
        const int64_t now_ms = SimClock::sim_millis();

        for (const auto& det : batch) {
            double x, y, z;
            polar_to_enu(det.range_m, det.azimuth_deg, det.elevation_deg,
                         ship.heading_deg, x, y, z);

            // nearest-neighbour association, gated on the PREDICTED position:
            // at a 3.2 s sweep a 250 m/s fighter moves ~800 m between
            // illuminations — gating on the stale position would fragment
            // every fast track (the gate is 750 m).
            Track* best = nullptr;
            double best_d2 = kGateM * kGateM;
            for (auto& tr : tracks) {
                const double dtg = std::max(0.02, (now_ms - tr.last_update_ms) / 1000.0);
                const double dx = tr.x + tr.vx * dtg - x;
                const double dy = tr.y + tr.vy * dtg - y;
                const double dz = tr.z + tr.vz * dtg - z;
                const double d2 = dx*dx + dy*dy + dz*dz;
                if (d2 < best_d2) { best_d2 = d2; best = &tr; }
            }

            if (best) {
                const double dt = std::max(0.02, (now_ms - best->last_update_ms) / 1000.0);
                // Track initiation: seed velocity from the first detection
                // pair so the predictor is useful from the second hit on.
                if (best->hits == 1 && dt > 0.5) {
                    best->vx = (x - best->x) / dt;
                    best->vy = (y - best->y) / dt;
                    best->vz = (z - best->z) / dt;
                }
                // alpha-beta filter
                const double rx = x - (best->x + best->vx * dt);
                const double ry = y - (best->y + best->vy * dt);
                const double rz = z - (best->z + best->vz * dt);
                best->x  += best->vx * dt + kAlpha * rx;
                best->y  += best->vy * dt + kAlpha * ry;
                best->z  += best->vz * dt + kAlpha * rz;
                best->vx += (kBeta / dt) * rx;
                best->vy += (kBeta / dt) * ry;
                best->vz += (kBeta / dt) * rz;
                best->hits++;
                best->quality = std::min(100, best->quality + 2);
                best->last_update_ms = now_ms;
            } else if (tracks.size() < kMaxTracks) {
                // Bounded id pool: recycle ids of dropped tracks so the keyed
                // TargetTrack topic tops out at kMaxTracks DDS instances
                // (an ever-incrementing key leaks one instance per track,
                // growing writer/reader memory without bound).
                int64_t new_id = -1;
                for (int k = 0; k < kMaxTracks; ++k) {
                    const int64_t cand = 1000 + (next_track_id_ + k) % kMaxTracks;
                    const bool in_use = std::any_of(tracks.begin(), tracks.end(),
                        [cand](const Track& tr) { return tr.id == cand; });
                    if (!in_use) { new_id = cand; next_track_id_ += k + 1; break; }
                }
                if (new_id < 0) continue; // pool exhausted; skip this blip
                Track t{};
                t.id = new_id;
                types::TargetTrack reg;
                reg.track_id = new_id;
                handles_[new_id] = writer_.register_instance(reg);
                t.x = x; t.y = y; t.z = z;
                t.vx = t.vy = t.vz = 0.0;
                t.hits = 1;
                t.quality = 30;
                t.classification =
                    static_cast<int>(types::TrackClassification::CLASS_UNKNOWN);
                t.last_update_ms = now_ms;
                tracks.push_back(t);
            }
        }

        // coast / drop + classify + publish
        for (auto it = tracks.begin(); it != tracks.end();) {
            if (now_ms - it->last_update_ms > kCoastMs) {
                auto h = handles_.find(it->id);
                if (h != handles_.end()) {
                    if (bus_.dispose_enabled.load())
                        writer_.dispose_instance(h->second);
                    handles_.erase(h);
                }
                it = tracks.erase(it);
                continue;
            }
            const double speed = std::sqrt(it->vx*it->vx + it->vy*it->vy + it->vz*it->vz);
            if (it->hits >= 3 &&
                it->classification ==
                    static_cast<int>(types::TrackClassification::CLASS_UNKNOWN))
                it->classification = classify(speed, it->z);

            it->history.push_back({it->x, it->y, it->z});
            if (it->history.size() > 10) it->history.pop_front();

            if (it->hits >= 2) { // publish confirmed tracks only
                types::TargetTrack msg;
                msg.track_id  = it->id;
                msg.timestamp = SimClock::stamp();

                msg.position.x_east_m  = it->x;
                msg.position.y_north_m = it->y;
                msg.position.z_up_m    = it->z;
                msg.velocity.x_east_m  = it->vx;
                msg.velocity.y_north_m = it->vy;
                msg.velocity.z_up_m    = it->vz;
                msg.acceleration.x_east_m  = 0.0;
                msg.acceleration.y_north_m = 0.0;
                msg.acceleration.z_up_m    = 0.0;

                msg.covariance.resize(types::COVARIANCE_SIZE, 0.0);
                msg.covariance[0] = msg.covariance[4] = msg.covariance[8] = 2500.0;
                msg.classification =
                    static_cast<types::TrackClassification>(it->classification);
                msg.quality = it->quality;
                writer_.write(msg);
            }
            ++it;
        }

        std::this_thread::sleep_until(next);
    }
}

} // namespace radar::app
