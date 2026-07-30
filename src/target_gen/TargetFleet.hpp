#pragma once
// TargetFleet owns both sides of target_gen's deliberately split DDS model:
//
// Simulation domain:
//   publishes: TargetGen/TargetTruth  (keyed per target_id, 50 Hz)
//   publishes: Ship/ShipPosition      (source_id = 1 ground truth, 10 Hz)
//
// Separate control domain:
//   subscribes: TargetControl/Request
//   publishes:  TargetControl/Reply, TargetControl/Snapshot

#include <atomic>
#include <thread>
#include <unordered_map>
#include <vector>

#include "DdsSupport.hpp"
#include "TargetScenario.hpp"
#include "TopicNames.hpp"

namespace target_gen {

class TargetFleet {
public:
    TargetFleet(int32_t domain_id, int32_t control_domain_id,
                int num_targets);
    ~TargetFleet() { stop(); }

    void start();
    void stop();

    // --respawn-range KM: recycle random targets whose ship-relative range
    // exceeds km (0 disables). Applies from the next loop cycle.
    void set_respawn_range_km(double km) {
        scenario_.set_respawn_range_km(km);
    }

private:
    void loop();
    bool process_control_requests();
    void publish_control_reply(
        const radar::types::TargetControlRequest& request,
        const ScenarioChange& change);
    void publish_control_snapshot();
    void dispose_targets(const std::vector<int32_t>& target_ids);
    void dispose_all_targets();

    int32_t domain_id_;
    int32_t control_domain_id_;

    dds::domain::DomainParticipant participant_;
    dds::pub::Publisher publisher_;
    dds::pub::DataWriter<radar::types::TargetTruth>
        truth_writer_{dds::core::null};
    dds::pub::DataWriter<radar::types::ShipPosition>
        ship_writer_{dds::core::null};
    std::unordered_map<int32_t, dds::core::InstanceHandle> truth_handles_;

    dds::domain::DomainParticipant control_participant_;
    dds::pub::Publisher control_publisher_;
    dds::sub::Subscriber control_subscriber_;
    dds::sub::DataReader<radar::types::TargetControlRequest>
        control_request_reader_{dds::core::null};
    dds::pub::DataWriter<radar::types::TargetControlReply>
        control_reply_writer_{dds::core::null};
    dds::pub::DataWriter<radar::types::TargetControlSnapshot>
        control_snapshot_writer_{dds::core::null};

    TargetScenario scenario_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
};

} // namespace target_gen
