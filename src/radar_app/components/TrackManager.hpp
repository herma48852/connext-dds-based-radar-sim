#pragma once
// TrackManager: DDS adapter over TrackerCore.
//
//   subscribes: Radar/DetectionEvent  (listener: enqueue)
//   publishes : Radar/TargetTrack     (10 Hz, alpha-beta filtered)
//
// All correlation/filter logic lives in TrackerCore (DDS-free; see
// tests/tracker_replay.cpp for the offline harness). This class converts
// DDS samples <-> core calls and manages instance handles for dispose.

#include <mutex>
#include <unordered_map>
#include <vector>

#include "ComponentBase.hpp"
#include "TrackerCore.hpp"
#include "../DataBus.hpp"

namespace radar::app {

class TrackManager : public ComponentBase {
public:
    TrackManager(int32_t domain_id, DataBus& bus)
        : ComponentBase(domain_id, "Radar.TrackManager"), bus_(bus) {}

    ~TrackManager() override { stop(); }

    void start() override;
    void stop() override;

private:
    void on_detection(const types::DetectionEvent& det);
    void update_loop();

    DataBus& bus_;
    dds::sub::DataReader<types::DetectionEvent> reader_{dds::core::null};
    dds::pub::DataWriter<types::TargetTrack>    writer_{dds::core::null};

    // Listener thread -> update thread handoff (mutex; batch copy is cheap)
    mutable std::mutex pending_mutex_;
    std::vector<types::DetectionEvent> pending_;

    TrackerCore core_;

    // track_id -> DDS instance handle (for dispose on drop/reset)
    std::unordered_map<int64_t, dds::core::InstanceHandle> handles_;
};

} // namespace radar::app
