#pragma once
// TrackerCore: DDS-free track-correlation core.
//
//   - nearest-neighbour association in ship-relative ENU
//   - alpha-beta position/velocity filter per track
//   - two-stage velocity initiation (cross-sweep endpoint seed)
//   - coast/drop with bounded id pool
//
// TrackManager (the DDS adapter) and tests/tracker_replay (offline harness,
// no Connext required) both drive this class. No DDS types, no SimClock:
// detections and time are passed in, dropped ids are returned for dispose.

#include <array>
#include <cstdint>
#include <deque>
#include <vector>

namespace radar::app {

struct CoreDetection {
    double range_m;        // ship-relative
    double azimuth_deg;    // ship-relative, 0 = bow, CW positive
    double elevation_deg;  // dwell elevation bar
};

struct CoreTrack {
    int64_t id;
    double x, y, z;          // ENU [m]
    double vx, vy, vz;       // ENU [m/s]
    bool   v_init;           // velocity seeded by cross-sweep hits
    int    cross_hits;       // associations with dt >= 1 s (init counter)
    double bx, by;           // birth position (endpoint velocity seed)
    int64_t birth_ms;
    int    hits;
    int    classification;   // matches types::TrackClassification ordinals
    int    quality;
    int64_t last_update_ms;
    std::deque<std::array<double,3>> history; // display trail (max 10)
};

class TrackerCore {
public:
    // One 10 Hz cycle. dets are ship-relative polar; heading converts to ENU.
    // Returns the ids of tracks dropped this cycle (adapter disposes them).
    std::vector<int64_t> update(const std::vector<CoreDetection>& dets,
                                double ship_heading_deg, int64_t now_ms);

    void reset();

    const std::vector<CoreTrack>& tracks() const { return tracks_; }

    // Classification ordinals (mirror types::TrackClassification)
    static constexpr int CLASS_UNKNOWN        = 0;
    static constexpr int CLASS_AIR_BREATHING  = 1;
    static constexpr int CLASS_BALLISTIC      = 2;
    static constexpr int CLASS_SURFACE        = 3;
    static constexpr int CLASS_CLUTTER        = 4;

    static constexpr double kGateM        = 750.0;
    static constexpr double kInitSpeedMps = 350.0; // capture-gate velocity uncertainty
    static constexpr double kAlpha        = 0.55;
    static constexpr double kBeta         = 0.20;
    // Must survive two fully missed bar visits: 3 bars x 1.6 s = 4.8 s
    // revisit, so 12 s covers an unlucky az dead-stripe crossing.
    static constexpr int64_t kCoastMs     = 12000;
    static constexpr int    kMaxTracks    = 256;
    // Merge radius for az-cell split duplicates: 2.25 deg dwell cells are
    // ~1.9 km at 50 km; 4 km covers second-adjacent splits. Velocity must
    // match only when both tracks have it seeded (kMergeDvMps).
    static constexpr double kMergeM     = 4000.0;
    static constexpr double kMergeDvMps = 30.0;

private:
    std::vector<CoreTrack> tracks_;
    int64_t next_track_id_{1000};
};

} // namespace radar::app
