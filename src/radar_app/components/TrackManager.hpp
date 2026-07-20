#pragma once
// TrackManager: correlates DetectionEvents into TargetTracks.
//
//   subscribes: Radar/DetectionEvent  (listener: enqueue, lock-free)
//   publishes : Radar/TargetTrack     (10 Hz, alpha-beta filtered)
//
// Simple but credible tracker:
//   - nearest-neighbour association in ship-relative ENU with a 750 m gate
//   - alpha-beta position/velocity filter per track
//   - tentative tracks promoted after 2 hits; dropped after 5 s coast
//   - classification heuristic from speed and altitude

#include <array>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "ComponentBase.hpp"
#include "../DataBus.hpp"

namespace radar::app {

class TrackManager : public ComponentBase {
public:
    TrackManager(int32_t domain_id, DataBus& bus)
        : ComponentBase(domain_id, "Radar.TrackManager"), bus_(bus) {}

    void start() override;

private:
    struct Track {
        int64_t id;
        double x, y, z;          // ENU [m]
        double vx, vy, vz;       // ENU [m/s]
        bool   v_init;           // velocity seeded by a cross-sweep hit
        int    hits;
        int    classification;
        int    quality;
        int64_t last_update_ms;
        std::deque<std::array<double,3>> history; // display trail (max 10)
    };

    static constexpr double kGateM        = 750.0;
    static constexpr double kInitSpeedMps = 350.0; // capture gate: velocity uncertainty of an un-seeded track
    static constexpr double kAlpha        = 0.55;
    static constexpr double kBeta         = 0.20;
    // Must exceed the worst-case revisit: 2 el bars x 1.6 s sweep = 3.2 s,
    // plus margin for a missed bar at the elevation-gate edge.
    static constexpr int64_t kCoastMs     = 9000;
    static constexpr int    kMaxTracks    = 256;

    void on_detection(const types::DetectionEvent& det);
    void update_loop();

    DataBus& bus_;
    dds::sub::DataReader<types::DetectionEvent> reader_{dds::core::null};
    dds::pub::DataWriter<types::TargetTrack>    writer_{dds::core::null};

    // Listener thread -> update thread handoff (mutex; batch copy is cheap)
    mutable std::mutex pending_mutex_;
    std::vector<types::DetectionEvent> pending_;

    int64_t next_track_id_{1000};

    // track_id -> DDS instance handle (for dispose on drop/reset)
    std::unordered_map<int64_t, dds::core::InstanceHandle> handles_;
};

} // namespace radar::app
