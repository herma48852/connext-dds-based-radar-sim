#include "TrackerCore.hpp"

#include <algorithm>
#include <cmath>

namespace radar::app {

namespace {
constexpr double kDeg2Rad = 3.14159265358979323846 / 180.0;

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
    if (speed > 300.0)            return TrackerCore::CLASS_BALLISTIC;
    if (speed > 100.0)            return TrackerCore::CLASS_AIR_BREATHING;
    if (speed < 30.0 && z < 50.0) return TrackerCore::CLASS_SURFACE;
    return TrackerCore::CLASS_UNKNOWN;
}
} // namespace

void TrackerCore::reset() {
    tracks_.clear();
    next_track_id_ = 1000;
}

std::vector<int64_t> TrackerCore::update(const std::vector<CoreDetection>& dets,
                                         double ship_heading_deg, int64_t now_ms) {
    for (const auto& det : dets) {
        double x, y, z;
        polar_to_enu(det.range_m, det.azimuth_deg, det.elevation_deg,
                     ship_heading_deg, x, y, z);

        // Nearest-neighbour association, gated in XY ONLY: reported z is
        // quantized to the elevation bar (z = R sin(el_bar)) and would
        // dominate a 3D gate. The gate grows with time-since-update while
        // velocity is unseeded or freshly seeded — azimuth is quantized to
        // 2.25 deg dwell cells (~1.2 km at 30 km), so early velocity
        // estimates are noisy.
        CoreTrack* best = nullptr;
        double best_d2 = 1e30;
        for (auto& tr : tracks_) {
            const double dtg = std::max(0.02, (now_ms - tr.last_update_ms) / 1000.0);
            double px = tr.x, py = tr.y, gate = kGateM;
            if (tr.v_init) {
                px += tr.vx * dtg; py += tr.vy * dtg;
                if (tr.cross_hits < 4) gate += 0.5 * kInitSpeedMps * dtg;
            } else {
                gate += kInitSpeedMps * dtg;
            }
            const double dx = px - x, dy = py - y;
            const double d2 = dx*dx + dy*dy;
            if (d2 < gate * gate && d2 < best_d2) { best_d2 = d2; best = &tr; }
        }

        if (best) {
            const double dt = std::max(0.02, (now_ms - best->last_update_ms) / 1000.0);
            // Track initiation: seed velocity only from the SECOND
            // cross-sweep hit, over the full birth-to-now span. A
            // single-pair seed is mostly cell-quantization noise and would
            // break the next predicted association.
            if (dt >= 1.0) {
                ++best->cross_hits;
                if (!best->v_init && best->cross_hits >= 2) {
                    const double span = std::max(1.0, (now_ms - best->birth_ms) / 1000.0);
                    double sx = (x - best->bx) / span;
                    double sy = (y - best->by) / span;
                    const double sp = std::hypot(sx, sy);
                    if (sp > 700.0) { sx *= 700.0 / sp; sy *= 700.0 / sp; }
                    best->vx = sx; best->vy = sy; best->vz = 0.0;
                    best->v_init = true;
                }
            }
            // alpha-beta filter. The VELOCITY (beta) update is applied only
            // to cross-sweep associations: within a dwell burst detections
            // are ~simultaneous, dt clamps to 0.02, and beta/dt = 10 turns
            // any residual into a multi-km/s velocity kick.
            const double rx = x - (best->x + best->vx * dt);
            const double ry = y - (best->y + best->vy * dt);
            const double rz = z - (best->z + best->vz * dt);
            best->x  += best->vx * dt + kAlpha * rx;
            best->y  += best->vy * dt + kAlpha * ry;
            best->z  += best->vz * dt + kAlpha * rz;
            if (dt >= 0.25) {
                best->vx += (kBeta / dt) * rx;
                best->vy += (kBeta / dt) * ry;
                best->vz += (kBeta / dt) * rz;
            }
            best->hits++;
            best->quality = std::min(100, best->quality + 2);
            best->last_update_ms = now_ms;
        } else if (tracks_.size() < (size_t)kMaxTracks) {
            // Bounded id pool: recycle ids of dropped tracks so the keyed
            // TargetTrack topic tops out at kMaxTracks DDS instances.
            int64_t new_id = -1;
            for (int k = 0; k < kMaxTracks; ++k) {
                const int64_t cand = 1000 + (next_track_id_ + k) % kMaxTracks;
                const bool in_use = std::any_of(tracks_.begin(), tracks_.end(),
                    [cand](const CoreTrack& tr) { return tr.id == cand; });
                if (!in_use) { new_id = cand; next_track_id_ += k + 1; break; }
            }
            if (new_id < 0) continue; // pool exhausted; skip this blip
            CoreTrack t{};
            t.id = new_id;
            t.x = x; t.y = y; t.z = z;
            t.vx = t.vy = t.vz = 0.0;
            t.bx = x; t.by = y; t.birth_ms = now_ms;
            t.hits = 1;
            t.quality = 30;
            t.classification = CLASS_UNKNOWN;
            t.last_update_ms = now_ms;
            tracks_.push_back(t);
        }
    }

    // coast / drop + classify
    std::vector<int64_t> dropped;
    for (auto it = tracks_.begin(); it != tracks_.end();) {
        if (now_ms - it->last_update_ms > kCoastMs) {
            dropped.push_back(it->id);
            it = tracks_.erase(it);
            continue;
        }
        const double speed = std::sqrt(it->vx*it->vx + it->vy*it->vy + it->vz*it->vz);
        if (it->hits >= 3 && it->classification == CLASS_UNKNOWN)
            it->classification = classify(speed, it->z);

        it->history.push_back({it->x, it->y, it->z});
        if (it->history.size() > 10) it->history.pop_front();
        ++it;
    }
    return dropped;
}

} // namespace radar::app
