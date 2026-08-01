#pragma once
// TrackerCore: DDS-free track-correlation core.
//
//   - range/angle nearest-neighbour association with range-scaled cross gate
//   - alpha-beta position/velocity filter per track
//   - three-of-five scan confirmation and endpoint velocity initiation
//   - state-preserving duplicate fusion at resolution-cell boundaries
//   - coast/drop with bounded id pool
//
// TrackManager (the DDS adapter) and tests/tracker_replay (offline harness,
// no Connext required) both drive this class. No DDS types, no SimClock:
// detections and time are passed in, dropped ids are returned for dispose.

#include <array>
#include <cstdint>
#include <deque>
#include <vector>

#include "DetectionIdentity.hpp"

namespace radar::app {

struct CoreDetection {
    double range_m;        // ship-relative
    double azimuth_deg;    // ship-relative, 0 = bow, CW positive
    double elevation_deg;  // dwell elevation bar
    double snr_db = 12.0;  // integrated reported S/N; see EffectiveRangeModel
    std::vector<DetectionIdentity> contributors;
};

struct CoreTrack {
    int64_t id;
    int64_t lifecycle_id;     // monotonic; never recycled by reset/id reuse
    double x, y, z;          // ENU [m]
    double vx, vy, vz;       // ENU [m/s]
    bool   v_init;           // velocity seeded by cross-sweep hits
    int    cross_hits;       // associations with dt >= 1 s (init counter)
    double bx, by;           // birth position (endpoint velocity seed)
    int64_t birth_ms;
    int    hits;
    bool   confirmed;
    int    classification;   // matches types::TrackClassification ordinals
    int    quality;
    int64_t last_update_ms;
    double range_stddev_m;
    double azimuth_stddev_deg;
    double elevation_stddev_deg;
    double last_detection_snr_db;
    double last_elevation_bar_deg;
    std::deque<int64_t> scan_hit_times;
    std::deque<std::array<double,3>> history; // display trail (max 10)
};

enum class CoreAssociationDecision {
    Initiate,
    Update,
    Reject,
    Merge,
    Drop
};

enum class CoreAssociationReason {
    None,
    Capacity,
    Duplicate,
    CoastTimeout,
    Reset,
    Shutdown
};

struct CoreAssociationEvent {
    CoreAssociationDecision decision{CoreAssociationDecision::Reject};
    CoreAssociationReason reason{CoreAssociationReason::None};
    bool has_measurement{false};
    CoreDetection measurement{};
    int64_t track_id{-1};
    int64_t track_lifecycle_id{-1};
    int64_t related_track_id{-1};
    int64_t related_track_lifecycle_id{-1};
    double innovation_score{0.0};
    int passing_candidate_count{0};
    bool track_confirmed{false};
    int64_t last_accepted_sim_millis{0};
};

class TrackerCore {
public:
    // One 10 Hz cycle. dets are ship-relative polar; heading converts to ENU.
    // Returns the ids of tracks dropped this cycle (adapter disposes them).
    std::vector<int64_t> update(const std::vector<CoreDetection>& dets,
                                double ship_heading_deg, int64_t now_ms);

    void reset();

    const std::vector<CoreTrack>& tracks() const { return tracks_; }
    const std::vector<CoreAssociationEvent>& association_events() const {
        return association_events_;
    }

    // Classification ordinals (mirror types::TrackClassification)
    static constexpr int CLASS_UNKNOWN        = 0;
    static constexpr int CLASS_AIR_BREATHING  = 1;
    static constexpr int CLASS_BALLISTIC      = 2;
    static constexpr int CLASS_SURFACE        = 3;
    static constexpr int CLASS_CLUTTER        = 4;

    // Range/angle measurement gate. The beam reports an SNR-weighted raster
    // bearing, not an exact monopulse angle, so the physical cross-range
    // allowance grows with target range.
    static constexpr double kRangeGateM         = 375.0;
    static constexpr double kAzimuthGateDeg     = 2.6;
    static constexpr double kCrossRangeFloorM   = 150.0;
    static constexpr double kInitSpeedMps = 350.0; // capture-gate velocity uncertainty
    static constexpr double kAlpha        = 0.55;
    static constexpr double kBeta         = 0.20;

    // A "hit" for confirmation must come from another scan visit, not another
    // pulse or overlapping adjacent beam. Three visits inside five nominal
    // 1.2-second face volumes confirm the track.
    static constexpr int64_t kIndependentScanMs = 600;
    static constexpr int64_t kConfirmationWindowMs = 6000;
    static constexpr int     kConfirmationHits = 3;
    static constexpr int64_t kTentativeCoastMs = 6000;

    // Four faces revisit a full three-bar volume every 1.2 s. The 12-second
    // coast deliberately survives many missed volumes during an outage.
    static constexpr int64_t kCoastMs     = 12000;
    static constexpr int    kMaxTracks    = 256;
    // Duplicate fragments must occupy the same range/angle resolution cell.
    // If one fragment is newer, its measurement is absorbed into the
    // survivor so merging cannot starve an established track of updates.
    static constexpr double kMergeRangeM       = 300.0;
    static constexpr double kMergeAzimuthDeg   = 3.3;
    static constexpr double kMergeDvMps        = 80.0;

private:
    std::vector<CoreTrack> tracks_;
    std::vector<CoreAssociationEvent> association_events_;
    int64_t next_track_id_{1000};
    int64_t next_lifecycle_id_{1};
};

} // namespace radar::app
