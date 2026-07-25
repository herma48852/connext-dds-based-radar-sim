// Offline replay of the detection -> tracking chain. NO DDS / Connext:
// replicates BeamScheduler (3-bar elevation raster, 2.25 deg per 10 ms
// dwell), the DetectionProcessor implant gates + noise + CFAR peak-pick,
// and drives the production TrackerCore. Prints track lifecycle so tracker
// changes can be verified without running the full app.
//
// Build (no Connext needed):  cmake --build build --target tracker_replay
// Run:                        ./build/tracker_replay [seconds]
// Regression:                 ./build/tracker_replay 300 --self-test --quiet

#include "BeamPatternModel.hpp"
#include "TrackerCore.hpp"
#include "DetectionModel.hpp"
#include "SearchRaster.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using radar::app::CoreDetection;
using radar::app::CoreTrack;
using radar::app::TrackerCore;

namespace {
constexpr double kDeg2Rad       = 3.14159265358979323846 / 180.0;
constexpr int    kRangeBins     = radar::app::detection_model::kRangeBins;
constexpr double kNoiseSigma    = radar::app::detection_model::kNoiseSigma;
constexpr double kCfarThreshold = radar::app::detection_model::kCfarThreshold;
constexpr double kDwellSec =
    radar::app::search_raster::kDwellPeriodSec;
constexpr int kPulsesPerDwell = static_cast<int>(
    radar::app::rf_model::kPulseRepetitionFrequencyHz * kDwellSec);
constexpr double kElGateDeg     = 5.5;

static_assert(kPulsesPerDwell == 10);

double wrap180(double a) { while (a > 180.0) a -= 360.0; while (a < -180.0) a += 360.0; return a; }

struct Truth {
    const char* name;
    double x, y, z;     // world ENU [m]
    double vx, vy, vz;  // [m/s]
    double rcs_dbsm;
};

// A standard engine has stable output, but std::normal_distribution does not.
// Summing 12 exact 24-bit uniforms gives a deterministic, zero-mean,
// unit-variance Gaussian approximation without implementation-defined state.
class DeterministicNormal {
public:
    DeterministicNormal(uint32_t seed, float sigma)
        : rng_(seed), sigma_(sigma) {}

    float next() {
        uint64_t sum = 0;
        for (int i = 0; i < 12; ++i)
            sum += rng_() >> 8;
        constexpr double kUnit24 = 1.0 / 16777216.0;
        return static_cast<float>((static_cast<double>(sum) * kUnit24 - 6.0)
                                  * static_cast<double>(sigma_));
    }

private:
    std::mt19937 rng_;
    float sigma_;
};

// Own ship: straight course, heading 45 deg, 20 kn (matches UI screenshot)
constexpr double kOwnHdgDeg = 45.0;
constexpr double kOwnSpeed  = 20.0 * 0.514444;
} // namespace

int main(int argc, char** argv) {
    double sim_s = 300.0;
    bool self_test = false;
    bool quiet = false;
    bool duration_seen = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--self-test") == 0) {
            self_test = true;
        } else if (std::strcmp(argv[i], "--quiet") == 0) {
            quiet = true;
        } else if (!duration_seen) {
            char* end = nullptr;
            const double parsed = std::strtod(argv[i], &end);
            if (!end || *end != '\0' || parsed <= 0.0) {
                std::fprintf(stderr,
                             "usage: %s [seconds] [--self-test] [--quiet]\n",
                             argv[0]);
                return 2;
            }
            sim_s = parsed;
            duration_seen = true;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }
    DeterministicNormal noise(42, static_cast<float>(kNoiseSigma));
    const auto nominal_pattern =
        radar::app::BeamPatternModel::calculate(0u);

    // Fleet: inbound profiles echoing target_gen (world ENU at t=0)
    std::vector<Truth> fleet = {
        // name      x        y        z      vx      vy    vz   rcs
        { "ship",   22000.0, -47000.0, 0.0,   0.0,   12.0, 0.0,  35.0 },
        { "fighter",-15000.0, 26000.0, 8000.0, 125.0, -216.7, 0.0,  0.0 },
        { "bomber", 46000.0, 76000.0, 10000.0, -90.0, -178.0, 0.0, 20.0 },
        { "decoy",  -42000.0, 30000.0, 7500.0,  195.0, -140.0, 0.0,  5.0 },
    };
    std::vector<int> illum(fleet.size(), 0); // gate passes per target

    double own_x = 0.0, own_y = 0.0;
    const double own_vx = kOwnSpeed * std::sin(kOwnHdgDeg * kDeg2Rad);
    const double own_vy = kOwnSpeed * std::cos(kOwnHdgDeg * kDeg2Rad);

    TrackerCore core;
    std::vector<CoreDetection> pending;
    double az = 0.0;
    int el_bar = 0;
    int64_t now_ms = 0;
    int64_t next_report_ms = 2000;
    int det_count = 0, births = 0, deaths = 0;
    size_t max_tracks = 0;
    bool id_pool_valid = true;
    std::vector<float> iq(2 * kRangeBins);
    std::vector<float> mag(kRangeBins);

    const int64_t total_dwells = (int64_t)(sim_s / kDwellSec);
    for (int64_t dwell = 0; dwell < total_dwells; ++dwell, now_ms += 10) {
        // --- BeamScheduler replica: advance bar per revolution ---
        const auto pointing =
            radar::app::search_raster::advance(az, el_bar);
        az = pointing.azimuth_deg;
        const double el_deg = pointing.elevation_deg;

        // --- own ship + fleet motion ---
        own_x += own_vx * kDwellSec; own_y += own_vy * kDwellSec;
        for (auto& t : fleet) { t.x += t.vx * kDwellSec; t.y += t.vy * kDwellSec; }

        // --- DetectionProcessor replica: PRF-rate pulses per search dwell ---
        for (int pulse = 0; pulse < kPulsesPerDwell; ++pulse) {
            for (auto& v : iq) v = noise.next();
            for (size_t ti = 0; ti < fleet.size(); ++ti) {
                const auto& t = fleet[ti];
                const double pulse_age =
                    pulse / radar::app::rf_model::kPulseRepetitionFrequencyHz;
                const double rx =
                    t.x - own_x + (t.vx - own_vx) * pulse_age;
                const double ry =
                    t.y - own_y + (t.vy - own_vy) * pulse_age;
                const double rz = t.z + t.vz * pulse_age;
                const double rxy = std::hypot(rx, ry);
                const double range =
                    std::sqrt(rx*rx + ry*ry + rz*rz);
                if (!radar::app::detection_model::
                        within_instrumented_range(range))
                    continue;
                const double az_world = std::atan2(rx, ry) / kDeg2Rad;
                const double az_ship  = wrap180(az_world - kOwnHdgDeg);
                const double beam_offset = wrap180(az_ship - az);
                if (std::fabs(beam_offset)
                    > nominal_pattern.beamwidth_3db_deg * 0.5)
                    continue;
                const double pattern_response =
                    nominal_pattern.relative_amplitude(beam_offset);
                const double el_t = std::atan2(rz, rxy) / kDeg2Rad;
                if (std::fabs(el_t - el_deg) > kElGateDeg) continue;
                ++illum[ti];
                const double amp =
                    radar::app::detection_model::target_voltage_amplitude(
                        t.rcs_dbsm, range, 1.0, pattern_response);
                const double phase =
                    radar::app::detection_model::
                        two_way_carrier_phase_rad(range);
                const double in_phase = std::cos(phase);
                const double quadrature = std::sin(phase);
                const int b0 =
                    radar::app::detection_model::range_bin_for(range);
                for (int db = -1; db <= 1; ++db) {
                    const int b = b0 + db;
                    if (b < 0 || b >= kRangeBins) continue;
                    const double w =
                        radar::app::detection_model::
                            compressed_pulse_weight(db);
                    iq[2*b] +=
                        static_cast<float>(amp * w * in_phase);
                    iq[2*b+1] +=
                        static_cast<float>(amp * w * quadrature);
                }
                (void)ti;
            }

            // --- CFAR peak-pick replica ---
            for (int i = 0; i < kRangeBins; ++i)
                mag[i] = std::sqrt(iq[2*i]*iq[2*i] + iq[2*i+1]*iq[2*i+1]);
            for (int i = 1; i < kRangeBins - 1; ++i) {
                if (mag[i] > kCfarThreshold && mag[i] >= mag[i-1] && mag[i] > mag[i+1]) {
                    pending.push_back(CoreDetection{
                        radar::app::detection_model::range_m_for_bin(i),
                        az,
                        el_deg});
                    ++det_count;
                }
            }
        }

        // --- 10 Hz tracker update ---
        if (dwell % 10 == 9) {
            const size_t before = core.tracks().size();
            const auto dropped = core.update(pending, kOwnHdgDeg, now_ms);
            pending.clear();
            deaths += (int)dropped.size();
            if (core.tracks().size() > before)
                births += (int)(core.tracks().size() - before);
            if (core.tracks().size() > max_tracks)
                max_tracks = core.tracks().size();
            for (const auto& track : core.tracks()) {
                if (track.id < 1000 || track.id >= 1000 + TrackerCore::kMaxTracks)
                    id_pool_valid = false;
            }
        }

        // --- report every 2 s ---
        if (!quiet && now_ms >= next_report_ms) {
            next_report_ms += 2000;
            std::printf("t=%5llds  det=%5d  trk=%2zu (births=%d deaths=%d)\n",
                        (long long)(now_ms/1000), det_count, core.tracks().size(),
                        births, deaths);
            for (const auto& tr : core.tracks()) {
                double best = 1e30; const Truth* bt = nullptr;
                for (const auto& ft : fleet) {
                    const double d = std::hypot(std::hypot(ft.x-own_x-tr.x,
                                                           ft.y-own_y-tr.y),
                                                ft.z-tr.z);
                    if (d < best) { best = d; bt = &ft; }
                }
                const double spd = std::sqrt(tr.vx*tr.vx + tr.vy*tr.vy + tr.vz*tr.vz);
                const double rng = std::hypot(tr.x, tr.y);
                const double azr = std::atan2(tr.x, tr.y) / kDeg2Rad;
                std::printf("   T%lld hits=%3d q=%3d v=%s crx=%d spd=%5.0f"
                            "  near=%s(%5.0fm) | TABLE: %.1fk %03.0f %3.0fm/s %4.0fm\n",
                            (long long)tr.id, tr.hits, tr.quality,
                            tr.v_init ? "Y" : "n", tr.cross_hits, spd,
                            bt ? bt->name : "-", best,
                            rng/1000.0, azr < 0 ? azr + 360.0 : azr, spd, tr.z);
            }
            std::printf("   illum:"); 
            for (size_t ti = 0; ti < fleet.size(); ++ti) {
                std::printf(" %s=%d", fleet[ti].name, illum[ti]);
                illum[ti] = 0;
            }
            std::printf("\n");
        }
    }
    std::printf("done. %d detections, %d births, %d deaths over %.0f s\n",
                det_count, births, deaths, sim_s);
    if (self_test) {
        bool ok = true;
        const auto check = [&](bool condition, const char* message) {
            if (!condition) {
                std::fprintf(stderr, "FAIL: %s\n", message);
                ok = false;
            }
        };
        check(std::fabs(sim_s - 300.0) < 1e-9,
              "tracker golden regression requires a 300 second replay");
        check(det_count == 2553, "expected 2553 deterministic detections");
        check(births == 29, "expected 29 deterministic track births");
        check(deaths == 106, "expected 106 deterministic track removals");
        check(max_tracks <= static_cast<size_t>(TrackerCore::kMaxTracks),
              "track count exceeded the bounded instance pool");
        check(id_pool_valid, "a track ID escaped the bounded 1000..1255 pool");

        TrackerCore coast_core;
        const auto coast_birth = coast_core.update(
            std::vector<CoreDetection>{{20000.0, 45.0, 3.0}}, 0.0, 0);
        check(coast_birth.empty() && coast_core.tracks().size() == 1,
              "coast boundary setup creates one live track");
        const auto at_boundary =
            coast_core.update({}, 0.0, TrackerCore::kCoastMs);
        check(at_boundary.empty() && coast_core.tracks().size() == 1,
              "track remains alive at the exact 12 second coast boundary");
        const auto after_boundary =
            coast_core.update({}, 0.0, TrackerCore::kCoastMs + 1);
        check(after_boundary.size() == 1 && coast_core.tracks().empty(),
              "track drops immediately after the 12 second coast boundary");

        if (!ok) return 1;
        std::printf("PASS: deterministic detection/tracker replay golden counts "
                    "and ID bounds\n");
    }
    return 0;
}
