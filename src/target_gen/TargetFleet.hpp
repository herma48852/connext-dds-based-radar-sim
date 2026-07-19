#pragma once
// TargetFleet: synthetic targets with configurable trajectories, RCS and
// kinematics, plus own-ship motion ground truth.
//
//   publishes: TargetGen/TargetTruth  (keyed per target_id, 50 Hz)
//   publishes: Ship/ShipPosition      (source_id = 1 ground truth, 10 Hz)
//
// Positions are computed in a local ENU frame anchored at the ship's start
// point; TargetTruth positions are ship-relative ENU (target - ship), which
// is exactly what the radar's DetectionProcessor needs to synthesize
// physically-plausible returns. Ship truth uses the same motion constants
// as the radar's INS so the two ShipPosition instances correlate.

#include <atomic>
#include <thread>
#include <vector>

#include "DdsSupport.hpp"
#include "TopicNames.hpp"

namespace target_gen {

class TargetFleet {
public:
    TargetFleet(int32_t domain_id, int num_targets);
    ~TargetFleet() { stop(); }

    void start();
    void stop();

private:
    struct Target {
        int32_t id;
        int32_t type;        // radar::types::TargetType
        double x, y, z;      // world ENU [m] (origin = ship start)
        double speed_mps;
        double heading_deg;
        double rcs_dbsm;
        int     maneuver;    // 0 = straight, 1 = weave, 2 = orbit
        double  phase;       // maneuver phase offset
    };

    // Same own-ship constants as the radar's ShipSimulator
    static constexpr double kShipHeadingDeg = 45.0;
    static constexpr double kShipSpeedMps   = 10.3;
    static constexpr double kShipStartLat   = 36.90;
    static constexpr double kShipStartLon   = -75.90;

    void loop();

    int32_t domain_id_;
    dds::domain::DomainParticipant participant_;
    dds::pub::Publisher            publisher_;
    dds::pub::DataWriter<radar::types::TargetTruth>  truth_writer_{dds::core::null};
    dds::pub::DataWriter<radar::types::ShipPosition> ship_writer_{dds::core::null};

    std::vector<Target> targets_;
    std::thread         thread_;
    std::atomic<bool>   stop_{false};
};

} // namespace target_gen
