#include "TrackerCore.hpp"

#include <algorithm>
#include <cmath>

#include "EffectiveRangeModel.hpp"

namespace radar::app {

namespace {
constexpr double kDeg2Rad = 3.14159265358979323846 / 180.0;
constexpr double kRad2Deg = 180.0 / 3.14159265358979323846;

double wrap180(double angle_deg) {
    while (angle_deg > 180.0) angle_deg -= 360.0;
    while (angle_deg < -180.0) angle_deg += 360.0;
    return angle_deg;
}

double azimuth_deg(double x, double y) {
    return std::atan2(x, y) * kRad2Deg;
}

double elevation_deg(double x, double y, double z) {
    return std::atan2(z, std::hypot(x, y)) * kRad2Deg;
}

double slant_range_m(double x, double y, double z) {
    return std::hypot(std::hypot(x, y), z);
}

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

int classify(double speed, double range_xy_m, double z) {
    if (speed > 300.0) return TrackerCore::CLASS_BALLISTIC;
    if (speed > 100.0) return TrackerCore::CLASS_AIR_BREATHING;
    // Surface contacts are only ever illuminated on the lowest (3 deg)
    // elevation bar (higher bars' gates start at 8.5 deg, above any
    // surface return), so a genuine surface track's reported z is
    // R*sin(3deg) ~ 0.05 R. A slow track higher than that is elevated
    // noise, not a ship. (Bare "speed < 30" misclassifies both elevated
    // noise AND fast movers whose velocity hasn't seeded yet.)
    if (speed < 30.0 && z < 0.07 * range_xy_m + 500.0)
        return TrackerCore::CLASS_SURFACE;
    return TrackerCore::CLASS_UNKNOWN;
}

void prune_scan_hits(CoreTrack& track, int64_t now_ms) {
    while (!track.scan_hit_times.empty() &&
           now_ms - track.scan_hit_times.front()
               > TrackerCore::kConfirmationWindowMs) {
        track.scan_hit_times.pop_front();
    }
}

bool record_scan_hit(CoreTrack& track, int64_t now_ms) {
    prune_scan_hits(track, now_ms);
    if (!track.scan_hit_times.empty() &&
        now_ms - track.scan_hit_times.back()
            < TrackerCore::kIndependentScanMs) {
        return false;
    }
    track.scan_hit_times.push_back(now_ms);
    if (static_cast<int>(track.scan_hit_times.size())
        >= TrackerCore::kConfirmationHits) {
        track.confirmed = true;
    }
    return true;
}

void absorb_duplicate(CoreTrack& survivor, const CoreTrack& fragment) {
    // A fragment created at a beam-cell transition contains the newest
    // measurement. Move the established track toward that measurement and,
    // crucially, carry its update time forward. Simply deleting the fragment
    // makes the established track coast out despite continuing detections.
    if (fragment.last_update_ms > survivor.last_update_ms) {
        const double dt = std::max(
            0.02,
            (fragment.last_update_ms - survivor.last_update_ms) / 1000.0);
        const double px = survivor.x + survivor.vx * dt;
        const double py = survivor.y + survivor.vy * dt;
        const double pz = survivor.z + survivor.vz * dt;
        survivor.x = px + TrackerCore::kAlpha * (fragment.x - px);
        survivor.y = py + TrackerCore::kAlpha * (fragment.y - py);
        survivor.z = pz + TrackerCore::kAlpha * (fragment.z - pz);
        survivor.last_update_ms = fragment.last_update_ms;
        survivor.range_stddev_m = fragment.range_stddev_m;
        survivor.azimuth_stddev_deg = fragment.azimuth_stddev_deg;
        survivor.elevation_stddev_deg = fragment.elevation_stddev_deg;
        survivor.last_detection_snr_db =
            fragment.last_detection_snr_db;
        survivor.last_elevation_bar_deg =
            fragment.last_elevation_bar_deg;
    }

    if (!survivor.v_init && fragment.v_init) {
        survivor.vx = fragment.vx;
        survivor.vy = fragment.vy;
        survivor.vz = fragment.vz;
        survivor.v_init = true;
    }
    survivor.cross_hits =
        std::max(survivor.cross_hits, fragment.cross_hits);
    survivor.hits = std::max(survivor.hits, fragment.hits);
    survivor.quality = std::max(survivor.quality, fragment.quality);
    survivor.confirmed = survivor.confirmed || fragment.confirmed;

    std::vector<int64_t> combined_hits;
    combined_hits.reserve(
        survivor.scan_hit_times.size()
        + fragment.scan_hit_times.size());
    combined_hits.insert(
        combined_hits.end(),
        survivor.scan_hit_times.begin(),
        survivor.scan_hit_times.end());
    combined_hits.insert(
        combined_hits.end(),
        fragment.scan_hit_times.begin(),
        fragment.scan_hit_times.end());
    std::sort(combined_hits.begin(), combined_hits.end());
    survivor.scan_hit_times.clear();
    for (const int64_t hit_ms : combined_hits) {
        if (survivor.scan_hit_times.empty() ||
            hit_ms - survivor.scan_hit_times.back()
                >= TrackerCore::kIndependentScanMs) {
            survivor.scan_hit_times.push_back(hit_ms);
        }
    }
    prune_scan_hits(survivor, survivor.last_update_ms);
    if (static_cast<int>(survivor.scan_hit_times.size())
        >= TrackerCore::kConfirmationHits) {
        survivor.confirmed = true;
    }
}
} // namespace

void TrackerCore::reset() {
    tracks_.clear();
    next_track_id_ = 1000;
}

std::vector<int64_t> TrackerCore::update(const std::vector<CoreDetection>& dets,
                                         double ship_heading_deg, int64_t now_ms) {
    for (const auto& det : dets) {
        const auto uncertainty =
            effective_range::measurement_uncertainty(
                det.snr_db,
                det.range_m);
        const double measured_azimuth_world_deg =
            det.azimuth_deg + ship_heading_deg;

        // Range/angle nearest-neighbour association. Elevation remains
        // excluded because it is the center of an 11-degree acceptance gate,
        // not a measured target elevation. The cross-range gate scales with
        // range, matching the physical uncertainty of a beam-centered plot.
        CoreTrack* best = nullptr;
        double best_score = 1e30;
        for (auto& tr : tracks_) {
            const double dtg = std::max(0.02, (now_ms - tr.last_update_ms) / 1000.0);
            double px = tr.x, py = tr.y, pz = tr.z;
            double motion_uncertainty_m = 0.0;
            if (tr.v_init) {
                px += tr.vx * dtg;
                py += tr.vy * dtg;
                pz += tr.vz * dtg;
                if (tr.cross_hits < 4)
                    motion_uncertainty_m =
                        0.5 * kInitSpeedMps * dtg;
            } else {
                motion_uncertainty_m = kInitSpeedMps * dtg;
            }

            const double predicted_range = slant_range_m(px, py, pz);
            const double measured_range = det.range_m;
            const double range_error =
                std::fabs(predicted_range - measured_range);
            const double azimuth_error_rad =
                std::fabs(wrap180(
                    azimuth_deg(px, py)
                    - measured_azimuth_world_deg))
                * kDeg2Rad;
            const double mean_range =
                0.5 * (predicted_range + measured_range);
            const double cross_range_error =
                mean_range * std::fabs(std::sin(azimuth_error_rad));
            const double range_gate =
                std::max(
                    kRangeGateM,
                    3.0 * uncertainty.range_stddev_m)
                + motion_uncertainty_m;
            const double azimuth_gate_deg =
                std::max(
                    kAzimuthGateDeg,
                    3.0 * uncertainty.azimuth_stddev_deg);
            const double cross_range_gate =
                kCrossRangeFloorM
                + mean_range
                    * std::tan(azimuth_gate_deg * kDeg2Rad)
                + motion_uncertainty_m;
            if (range_error >= range_gate ||
                cross_range_error >= cross_range_gate) {
                continue;
            }
            const double score =
                (range_error / range_gate)
                    * (range_error / range_gate)
                + (cross_range_error / cross_range_gate)
                    * (cross_range_error / cross_range_gate);
            if (score < best_score) {
                best_score = score;
                best = &tr;
            }
        }

        if (best) {
            const double dt = std::max(0.02, (now_ms - best->last_update_ms) / 1000.0);
            const double px = best->x + best->vx * dt;
            const double py = best->y + best->vy * dt;
            const double pz = best->z + best->vz * dt;
            const double predicted_elevation_deg =
                elevation_deg(px, py, pz);

            // The elevation value is the center of an 11-degree acceptance
            // bar, not a measured angle. Treat it as interval-censored data:
            // retain the predicted elevation while it lies inside the bar,
            // otherwise project it to the nearest boundary. For a 14-to-25
            // degree handoff this selects their physical 19.5-degree boundary
            // instead of inventing an 11-degree target jump.
            const double effective_elevation_deg = std::clamp(
                predicted_elevation_deg,
                det.elevation_deg
                    - effective_range::kElevationBarHalfWidthDeg,
                det.elevation_deg
                    + effective_range::kElevationBarHalfWidthDeg);
            double x, y, z;
            polar_to_enu(
                det.range_m,
                det.azimuth_deg,
                effective_elevation_deg,
                ship_heading_deg,
                x, y, z);
            const bool elevation_bar_changed =
                std::fabs(
                    det.elevation_deg
                    - best->last_elevation_bar_deg) > 0.1;

            // Track initiation: seed velocity only from the SECOND
            // cross-sweep hit, over the full birth-to-now span. A
            // single-pair seed is mostly cell-quantization noise and would
            // break the next predicted association.
            if (elevation_bar_changed && !best->v_init) {
                // A bar handoff changes the interval-censored elevation
                // reference. Restart the velocity baseline so that change is
                // never mistaken for target translation during initiation.
                best->bx = x;
                best->by = y;
                best->birth_ms = now_ms;
                best->cross_hits = 0;
            } else if (dt >= 1.0) {
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
            const double rx = x - px;
            const double ry = y - py;
            const double rz = z - pz;
            const double position_alpha =
                elevation_bar_changed ? 1.0 : kAlpha;
            best->x = px + position_alpha * rx;
            best->y = py + position_alpha * ry;
            best->z = pz + position_alpha * rz;
            if (dt >= 0.25 && !elevation_bar_changed) {
                best->vx += (kBeta / dt) * rx;
                best->vy += (kBeta / dt) * ry;
                best->vz += (kBeta / dt) * rz;
            }
            best->hits++;
            const bool independent_scan =
                record_scan_hit(*best, now_ms);
            best->quality = std::min(
                100,
                best->quality + (independent_scan ? 12 : 1));
            best->last_update_ms = now_ms;
            best->range_stddev_m = uncertainty.range_stddev_m;
            best->azimuth_stddev_deg =
                uncertainty.azimuth_stddev_deg;
            best->elevation_stddev_deg =
                uncertainty.elevation_stddev_deg;
            best->last_detection_snr_db = det.snr_db;
            best->last_elevation_bar_deg = det.elevation_deg;
        } else if (tracks_.size() < (size_t)kMaxTracks) {
            double x, y, z;
            polar_to_enu(
                det.range_m,
                det.azimuth_deg,
                det.elevation_deg,
                ship_heading_deg,
                x, y, z);
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
            t.confirmed = false;
            t.quality = 30;
            t.classification = CLASS_UNKNOWN;
            t.last_update_ms = now_ms;
            t.range_stddev_m = uncertainty.range_stddev_m;
            t.azimuth_stddev_deg = uncertainty.azimuth_stddev_deg;
            t.elevation_stddev_deg =
                uncertainty.elevation_stddev_deg;
            t.last_detection_snr_db = det.snr_db;
            t.last_elevation_bar_deg = det.elevation_deg;
            t.scan_hit_times.push_back(now_ms);
            tracks_.push_back(t);
        }
    }

    // Fuse fragments that occupy the same radar resolution cell. This is a
    // safety net behind dwell-plot clustering and the range/angle gate.
    // Unlike the former delete-only merge, absorb_duplicate transfers a newer
    // measurement and timestamp into the established survivor.
    std::vector<int64_t> dropped;
    for (size_t i = 0; i < tracks_.size(); ++i) {
        for (size_t j = i + 1; j < tracks_.size();) {
            const auto& a = tracks_[i];
            const auto& b = tracks_[j];
            const double range_a = slant_range_m(a.x, a.y, a.z);
            const double range_b = slant_range_m(b.x, b.y, b.z);
            if (std::fabs(range_a - range_b) >= kMergeRangeM ||
                std::fabs(wrap180(
                    azimuth_deg(a.x, a.y)
                    - azimuth_deg(b.x, b.y)))
                    >= kMergeAzimuthDeg) {
                ++j;
                continue;
            }
            if (a.v_init && b.v_init) {
                const double dvx = a.vx - b.vx, dvy = a.vy - b.vy;
                if (dvx*dvx + dvy*dvy
                    >= kMergeDvMps*kMergeDvMps) {
                    ++j;
                    continue;
                }
            }

            const bool prefer_j =
                (b.confirmed && !a.confirmed)
                || (b.confirmed == a.confirmed && b.hits > a.hits);
            if (prefer_j)
                std::swap(tracks_[i], tracks_[j]);
            dropped.push_back(tracks_[j].id);
            absorb_duplicate(tracks_[i], tracks_[j]);
            tracks_.erase(tracks_.begin()
                          + static_cast<std::ptrdiff_t>(j));
        }
    }

    // coast / drop + classify
    for (auto it = tracks_.begin(); it != tracks_.end();) {
        prune_scan_hits(*it, now_ms);
        const int64_t coast_ms =
            it->confirmed ? kCoastMs : kTentativeCoastMs;
        if (now_ms - it->last_update_ms > coast_ms) {
            dropped.push_back(it->id);
            it = tracks_.erase(it);
            continue;
        }
        const double speed = std::sqrt(it->vx*it->vx + it->vy*it->vy + it->vz*it->vz);
        // Re-evaluate every cycle: velocity seeds on the SECOND cross-sweep
        // hit, so a track classified at birth (v unseeded) would otherwise
        // be stuck forever (fast movers read SURF/UNK at 240+ m/s).
        if (it->confirmed)
            it->classification = classify(speed, std::hypot(it->x, it->y), it->z);

        it->history.push_back({it->x, it->y, it->z});
        if (it->history.size() > 10) it->history.pop_front();
        ++it;
    }
    return dropped;
}

} // namespace radar::app
