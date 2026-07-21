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

#include "DdsSupport.hpp"
#include "TargetScenario.hpp"
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
    void set_respawn_range_km(double km) { scenario_.set_respawn_range_km(km); }

private:
    void loop();

    int32_t domain_id_;
    dds::domain::DomainParticipant participant_;
    dds::pub::Publisher            publisher_;
    dds::pub::DataWriter<radar::types::TargetTruth>  truth_writer_{dds::core::null};
    dds::pub::DataWriter<radar::types::ShipPosition> ship_writer_{dds::core::null};

    TargetScenario      scenario_;
    std::thread         thread_;
    std::atomic<bool>   stop_{false};
};

} // namespace target_gen
