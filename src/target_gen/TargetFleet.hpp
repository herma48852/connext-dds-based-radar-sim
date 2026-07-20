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
#include <random>
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

    // --respawn-range KM: recycle targets whose ship-relative range
    // exceeds km (0 disables). Applies from the next loop cycle.
    void set_respawn_range_km(double km) { respawn_range_m_ = km * 1000.0; }

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
        int     profile;     // index into kProfiles (respawn resets altitude)
    };

    // Same own-ship constants as the radar's ShipSimulator
    static constexpr double kShipHeadingDeg = 45.0;
    static constexpr double kShipSpeedMps   = 10.3;
    static constexpr double kShipStartLat   = 36.90;
    static constexpr double kShipStartLon   = -75.90;

    void loop();
    // Recycle a target that left coverage: fresh inbound trajectory at
    // long range around the ship's CURRENT position (keeps the demo
    // picture busy; targets otherwise fly straight away forever).
    void respawn(Target& t, double ship_e, double ship_n);

    int32_t domain_id_;
    dds::domain::DomainParticipant participant_;
    dds::pub::Publisher            publisher_;
    dds::pub::DataWriter<radar::types::TargetTruth>  truth_writer_{dds::core::null};
    dds::pub::DataWriter<radar::types::ShipPosition> ship_writer_{dds::core::null};

    std::vector<Target> targets_;
    std::thread         thread_;
    std::atomic<bool>   stop_{false};

    std::mt19937_64 rng_{20260719};
    double respawn_range_m_ = 120000.0; // 0 disables recycling
};

} // namespace target_gen
