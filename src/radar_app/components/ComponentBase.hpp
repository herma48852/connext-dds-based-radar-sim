#pragma once
// ============================================================================
// Base class for radar components. Each component owns its OWN
// DomainParticipant (created from RadarParticipantProfile) with a
// descriptive entity name, so Connext Studio's topology map shows
// "Radar.BeamScheduler", "Radar.TrackManager", ... as separate nodes.
//
// NOTE: splitting a process into multiple participants is a deliberate
// demo choice (maximum topology visibility). A production system would
// typically use one participant with several publishers/subscribers.
// ============================================================================

#include <atomic>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "DdsSupport.hpp"
#include "TopicNames.hpp"

namespace radar::app {

class ComponentBase {
public:
    ComponentBase(int32_t domain_id, std::string entity_name)
        : participant_(radds::make_participant(
              domain_id, dds_names::PROFILE_RADAR_PARTICIPANT, std::move(entity_name))),
          publisher_(participant_),
          subscriber_(participant_) {}

    virtual ~ComponentBase() { stop(); }

    virtual void start() = 0;

    virtual void stop() {
        stop_.store(true);
        join_all();
    }

    const dds::domain::DomainParticipant& participant() const { return participant_; }

protected:
    template <typename F>
    void spawn(F&& fn) { threads_.emplace_back(std::forward<F>(fn)); }

    void join_all() {
        for (auto& t : threads_)
            if (t.joinable()) t.join();
        threads_.clear();
    }

    dds::domain::DomainParticipant participant_;
    dds::pub::Publisher            publisher_;
    dds::sub::Subscriber           subscriber_;
    std::atomic<bool>              stop_{false};

private:
    std::vector<std::thread> threads_;
};

} // namespace radar::app
