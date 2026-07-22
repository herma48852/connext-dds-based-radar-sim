// Offline replay of the detection -> tracking chain. NO DDS / Connext:
// replicates BeamScheduler (2-bar elevation raster, 2.25 deg per 10 ms
// dwell), the DetectionProcessor implant gates + noise + CFAR peak-pick,
// and drives the production TrackerCore. Prints track lifecycle so tracker
// changes can be verified without running the full app.
//
// Build (no Connext needed):  cmake --build build --target tracker_replay
// Run:                        ./build/tracker_replay [seconds]
// Regression:                 ./build/tracker_replay 300 --self-test --quiet

#include "TrackerCore.hpp"

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
constexpr int    kRangeBins     = 512;
constexpr double kRangeMaxM     = 100000.0;
constexpr double kNoiseSigma    = 0.05;
constexpr double kCfarThreshold = 0.26;
constexpr double kSignalScale   = 2.0e8;
constexpr double kBeamwidthDeg  = 2.0;
constexpr double kAzStepDeg     = 2.25;
constexpr double kDwellSec      = 0.01;
constexpr int    kNumElBars     = 3;
constexpr double kElBars[kNumElBars] = {3.0, 14.0, 25.0};
constexpr double kElGateDeg     = 5.5;

double wrap180(double a) { while (a > 180.0) a -= 360.0; while (a < -180.0) a += 360.0; return a; }
double wrap360(double a) { while (a >= 360.0) a -= 360.0; while (a < 0.0) a += 360.0; return a; }

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
        if (az + kAzStepDeg >= 360.0) el_bar = (el_bar + 1) % kNumElBars;
        az = wrap360(az + kAzStepDeg);
        const double el_deg = kElBars[el_bar];

        // --- own ship + fleet motion ---
        own_x += own_vx * kDwellSec; own_y += own_vy * kDwellSec;
        for (auto& t : fleet) { t.x += t.vx * kDwellSec; t.y += t.vy * kDwellSec; }

        // --- DetectionProcessor replica: 10 x 1 kHz pulses per 10 ms dwell ---
        for (int pulse = 0; pulse < 10; ++pulse) {
            for (auto& v : iq) v = noise.next();
            for (size_t ti = 0; ti < fleet.size(); ++ti) {
                const auto& t = fleet[ti];
                const double rx = t.x - own_x, ry = t.y - own_y;
                const double rxy = std::hypot(rx, ry);
                const double range = std::sqrt(rx*rx + ry*ry + t.z*t.z);
                if (range < 100.0 || range > kRangeMaxM) continue;
                const double az_world = std::atan2(rx, ry) / kDeg2Rad;
                const double az_ship  = wrap180(az_world - kOwnHdgDeg);
                if (std::fabs(wrap180(az_ship - az)) > kBeamwidthDeg * 0.5) continue;
                const double el_t = std::atan2(t.z, rxy) / kDeg2Rad;
                if (std::fabs(el_t - el_deg) > kElGateDeg) continue;
                ++illum[ti];
                const double rcs_lin = std::pow(10.0, t.rcs_dbsm / 10.0);
                const double amp = kSignalScale * std::sqrt(rcs_lin) / (range * range);
                const int b0 = (int)(range / kRangeMaxM * kRangeBins);
                for (int db = -1; db <= 1; ++db) {
                    const int b = b0 + db;
                    if (b < 0 || b >= kRangeBins) continue;
                    const double w = (db == 0) ? 1.0 : 0.4;
                    iq[2*b]   += (float)(amp * w);
                    iq[2*b+1] += (float)(amp * w * 0.3);
                }
                (void)ti;
            }

            // --- CFAR peak-pick replica ---
            for (int i = 0; i < kRangeBins; ++i)
                mag[i] = std::sqrt(iq[2*i]*iq[2*i] + iq[2*i+1]*iq[2*i+1]);
            for (int i = 1; i < kRangeBins - 1; ++i) {
                if (mag[i] > kCfarThreshold && mag[i] >= mag[i-1] && mag[i] > mag[i+1]) {
                    pending.push_back(CoreDetection{
                        (double)i / kRangeBins * kRangeMaxM, az, el_deg});
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
        check(det_count == 1562, "expected 1562 deterministic detections");
        check(births == 27, "expected 27 deterministic track births");
        check(deaths == 34, "expected 34 deterministic track deaths");
        check(max_tracks <= static_cast<size_t>(TrackerCore::kMaxTracks),
              "track count exceeded the bounded instance pool");
        check(id_pool_valid, "a track ID escaped the bounded 1000..1255 pool");
        if (!ok) return 1;
        std::printf("PASS: deterministic detection/tracker replay golden counts "
                    "and ID bounds\n");
    }
    return 0;
}
