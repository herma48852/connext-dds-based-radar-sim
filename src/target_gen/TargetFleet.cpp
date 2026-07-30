#include "TargetFleet.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <utility>

#include "PeriodicDeadline.hpp"
#include "SimClock.hpp"
#include "WorkerGuard.hpp"

namespace target_gen {

using TT = radar::types::TargetType;

namespace {
constexpr double kDeg2Rad = 3.14159265358979323846 / 180.0;

static_assert(static_cast<int32_t>(ScenarioTargetType::Fighter) ==
              static_cast<int32_t>(TT::TARGET_FIGHTER));
static_assert(static_cast<int32_t>(ScenarioTargetType::Bomber) ==
              static_cast<int32_t>(TT::TARGET_BOMBER));
static_assert(static_cast<int32_t>(ScenarioTargetType::Missile) ==
              static_cast<int32_t>(TT::TARGET_MISSILE));
static_assert(static_cast<int32_t>(ScenarioTargetType::Ship) ==
              static_cast<int32_t>(TT::TARGET_SHIP));
static_assert(static_cast<int32_t>(ScenarioTargetType::Drone) ==
              static_cast<int32_t>(TT::TARGET_DRONE));
static_assert(static_cast<int32_t>(ScenarioTargetType::Decoy) ==
              static_cast<int32_t>(TT::TARGET_DECOY));
} // namespace

TargetFleet::TargetFleet(int32_t domain_id, int32_t control_domain_id,
                         int num_targets)
    : domain_id_(domain_id),
      control_domain_id_(control_domain_id),
      participant_(radds::make_participant(
          domain_id, radar::dds_names::PROFILE_TARGETGEN_PARTICIPANT,
          "TargetGen.Generator")),
      publisher_(participant_),
      control_participant_(radds::make_participant(
          control_domain_id,
          radar::dds_names::PROFILE_TARGET_CONTROL_PARTICIPANT,
          "TargetGen.Control")),
      control_publisher_(control_participant_),
      control_subscriber_(control_participant_),
      scenario_(num_targets) {}

void TargetFleet::start() {
    auto truth_topic = radds::make_topic<radar::types::TargetTruth>(
        participant_, radar::dds_names::TOPIC_TARGET_TRUTH);
    auto ship_topic = radds::make_topic<radar::types::ShipPosition>(
        participant_, radar::dds_names::TOPIC_SHIP_POSITION);
    truth_writer_ = radds::make_writer<radar::types::TargetTruth>(
        publisher_, truth_topic, radar::dds_names::PROFILE_TARGET_TRUTH);
    ship_writer_ = radds::make_writer<radar::types::ShipPosition>(
        publisher_, ship_topic, radar::dds_names::PROFILE_SHIP_POSITION);

    auto request_topic =
        radds::make_topic<radar::types::TargetControlRequest>(
            control_participant_,
            radar::dds_names::TOPIC_TARGET_CONTROL_REQUEST);
    auto reply_topic =
        radds::make_topic<radar::types::TargetControlReply>(
            control_participant_,
            radar::dds_names::TOPIC_TARGET_CONTROL_REPLY);
    auto snapshot_topic =
        radds::make_topic<radar::types::TargetControlSnapshot>(
            control_participant_,
            radar::dds_names::TOPIC_TARGET_CONTROL_SNAPSHOT);
    control_request_reader_ =
        radds::make_reader<radar::types::TargetControlRequest>(
            control_subscriber_, request_topic,
            radar::dds_names::PROFILE_TARGET_CONTROL_REQUEST);
    control_reply_writer_ =
        radds::make_writer<radar::types::TargetControlReply>(
            control_publisher_, reply_topic,
            radar::dds_names::PROFILE_TARGET_CONTROL_REPLY);
    control_snapshot_writer_ =
        radds::make_writer<radar::types::TargetControlSnapshot>(
            control_publisher_, snapshot_topic,
            radar::dds_names::PROFILE_TARGET_CONTROL_SNAPSHOT);

    thread_ = std::thread([this] {
        radar::run_worker_guarded(
            "TargetGen.Generator", [this] { loop(); });
    });
}

void TargetFleet::stop() {
    stop_.store(true);
    if (thread_.joinable())
        thread_.join();
    dispose_all_targets();
}

void TargetFleet::dispose_targets(
        const std::vector<int32_t>& target_ids) {
    for (const int32_t id : target_ids) {
        const auto handle = truth_handles_.find(id);
        if (handle == truth_handles_.end())
            continue;
        truth_writer_.dispose_instance(handle->second);
        truth_handles_.erase(handle);
    }
}

void TargetFleet::dispose_all_targets() {
    if (truth_writer_ != dds::core::null) {
        for (const auto& [id, handle] : truth_handles_) {
            (void)id;
            truth_writer_.dispose_instance(handle);
        }
    }
    truth_handles_.clear();
}

void TargetFleet::publish_control_reply(
        const radar::types::TargetControlRequest& request,
        const ScenarioChange& change) {
    radar::types::TargetControlReply reply;
    reply.client_id = request.client_id;
    reply.request_id = request.request_id;
    reply.timestamp = radar::SimClock::stamp();
    reply.result = change.accepted
        ? radar::types::TargetControlResult::TARGET_CONTROL_ACCEPTED
        : radar::types::TargetControlResult::TARGET_CONTROL_REJECTED;
    reply.scenario_instance_id = change.scenario_instance_id;
    reply.message = change.message;
    control_reply_writer_.write(reply);
}

bool TargetFleet::process_control_requests() {
    bool mutated = false;
    radar::types::TargetControlRequest request;
    dds::sub::SampleInfo info;
    for (int i = 0;
         i < 64 &&
         control_request_reader_.extensions().take(request, info); ++i) {
        if (!info.valid())
            continue;

        ScenarioChange change;
        switch (request.action) {
        case radar::types::TargetControlAction::TARGET_CONTROL_ADD_SCENARIO:
            change = scenario_.add_scenario(
                request.scenario_template, request.target_count);
            break;
        case radar::types::TargetControlAction::TARGET_CONTROL_REMOVE_SCENARIO:
            change = scenario_.remove_scenario(
                request.scenario_instance_id);
            break;
        case radar::types::TargetControlAction::TARGET_CONTROL_REMOVE_TARGET:
            change = scenario_.remove_target(request.target_id);
            break;
        case radar::types::TargetControlAction::TARGET_CONTROL_CLEAR_ALL:
            change = scenario_.clear_all();
            break;
        default:
            change.message = "unsupported target-control action";
            break;
        }

        dispose_targets(change.removed_target_ids);
        publish_control_reply(request, change);
        mutated = mutated || change.accepted;
        std::cout << "[target_gen] control request " << request.request_id
                  << ": " << change.message;
        if (change.scenario_instance_id != 0)
            std::cout << " (scenario "
                      << change.scenario_instance_id << ")";
        std::cout << "\n";
    }
    return mutated;
}

void TargetFleet::publish_control_snapshot() {
    radar::types::TargetControlSnapshot snapshot;
    snapshot.generator_id = 1;
    snapshot.timestamp = radar::SimClock::stamp();
    snapshot.revision = scenario_.revision();

    snapshot.catalog.reserve(TargetScenario::catalog().size());
    for (const auto& entry : TargetScenario::catalog()) {
        radar::types::TargetScenarioTemplateDescriptor descriptor;
        descriptor.name = entry.name;
        descriptor.label = entry.label;
        descriptor.description = entry.description;
        descriptor.configurable_target_count =
            entry.configurable_target_count;
        descriptor.default_target_count = entry.default_target_count;
        descriptor.minimum_target_count = entry.minimum_target_count;
        descriptor.maximum_target_count = entry.maximum_target_count;
        snapshot.catalog.push_back(std::move(descriptor));
    }

    snapshot.scenarios.reserve(scenario_.scenarios().size());
    for (const auto& active : scenario_.scenarios()) {
        radar::types::TargetScenarioInstanceState state;
        state.scenario_instance_id = active.id;
        state.template_name = active.template_name;
        state.target_count = static_cast<int32_t>(
            scenario_.scenario_target_count(active.id));
        snapshot.scenarios.push_back(std::move(state));
    }

    const double ship_velocity_e =
        TargetScenario::kShipSpeedMps *
        std::sin(TargetScenario::kShipHeadingDeg * kDeg2Rad);
    const double ship_velocity_n =
        TargetScenario::kShipSpeedMps *
        std::cos(TargetScenario::kShipHeadingDeg * kDeg2Rad);
    snapshot.targets.reserve(scenario_.targets().size());
    for (const auto& target : scenario_.targets()) {
        radar::types::TargetControlTargetState state;
        state.target_id = target.id;
        state.scenario_instance_id = target.scenario_instance_id;
        state.target_type =
            static_cast<radar::types::TargetType>(target.type);
        state.position.x_east_m =
            target.x - scenario_.ship_east_m();
        state.position.y_north_m =
            target.y - scenario_.ship_north_m();
        state.position.z_up_m = target.z;
        state.velocity.x_east_m =
            target.velocity_x_mps - ship_velocity_e;
        state.velocity.y_north_m =
            target.velocity_y_mps - ship_velocity_n;
        state.velocity.z_up_m = 0.0;
        state.range_m = std::hypot(
            std::hypot(
                state.position.x_east_m,
                state.position.y_north_m),
            state.position.z_up_m);
        snapshot.targets.push_back(std::move(state));
    }
    control_snapshot_writer_.write(snapshot);
}

void TargetFleet::loop() {
    using namespace std::chrono;
    constexpr double kDt = 0.02; // 50 Hz truth
    constexpr double kMetersPerDegLat = 111320.0;

    double lat = TargetScenario::kShipStartLat;
    double lon = TargetScenario::kShipStartLon;
    auto next = steady_clock::now();
    int cycle = 0;

    while (!stop_.load()) {
        next = radar::advance_periodic_deadline(
            next, milliseconds(20));
        const double t = radar::SimClock::sim_millis() / 1000.0;

        const bool mutated = process_control_requests();
        const ScenarioStep step = scenario_.step(kDt, t);
        dispose_targets(step.removed_target_ids);
        for (const int32_t id : step.respawned_target_ids) {
            std::cout << "[target_gen] target " << id
                      << " respawned inbound (range > "
                      << scenario_.respawn_range_km() << " km)\n";
        }
        for (const int64_t id : step.completed_scenario_ids) {
            std::cout << "[target_gen] one-shot scenario " << id
                      << " completed\n";
        }

        const double ship_e = scenario_.ship_east_m();
        const double ship_n = scenario_.ship_north_m();
        lat = TargetScenario::kShipStartLat +
              ship_n / kMetersPerDegLat;
        lon = TargetScenario::kShipStartLon +
              ship_e /
                  (kMetersPerDegLat * std::cos(lat * kDeg2Rad));

        for (const auto& target : scenario_.targets()) {
            radar::types::TargetTruth message;
            message.target_id = target.id;
            message.timestamp = radar::SimClock::stamp();
            message.position.x_east_m = target.x - ship_e;
            message.position.y_north_m = target.y - ship_n;
            message.position.z_up_m = target.z;
            message.velocity.x_east_m =
                target.velocity_x_mps -
                TargetScenario::kShipSpeedMps *
                    std::sin(
                        TargetScenario::kShipHeadingDeg * kDeg2Rad);
            message.velocity.y_north_m =
                target.velocity_y_mps -
                TargetScenario::kShipSpeedMps *
                    std::cos(
                        TargetScenario::kShipHeadingDeg * kDeg2Rad);
            message.velocity.z_up_m = 0.0;
            message.acceleration.x_east_m = 0.0;
            message.acceleration.y_north_m = 0.0;
            message.acceleration.z_up_m = 0.0;
            message.rcs_dbsm = target.rcs_dbsm;
            message.target_type =
                static_cast<radar::types::TargetType>(target.type);

            auto handle = truth_handles_.find(target.id);
            if (handle == truth_handles_.end()) {
                handle = truth_handles_.emplace(
                    target.id,
                    truth_writer_.register_instance(message)).first;
            }
            truth_writer_.write(message, handle->second);
        }

        // Ship truth at 10 Hz (every 5th cycle).
        if (++cycle % 5 == 0) {
            radar::types::ShipPosition ship;
            ship.source_id = 1;
            ship.timestamp = radar::SimClock::stamp();
            ship.latitude_deg = lat;
            ship.longitude_deg = lon;
            ship.altitude_m = 0.0;
            ship.heading_deg = TargetScenario::kShipHeadingDeg;
            ship.course_deg = TargetScenario::kShipHeadingDeg;
            ship.speed_mps = TargetScenario::kShipSpeedMps;
            ship.pitch_deg = 0.8 * std::sin(t * 0.50);
            ship.roll_deg = 2.5 * std::sin(t * 0.31 + 1.2);
            ship_writer_.write(ship);
        }

        // Inventory positions refresh at 5 Hz; mutations publish immediately.
        if (mutated || !step.completed_scenario_ids.empty() ||
            cycle % 10 == 0) {
            publish_control_snapshot();
        }

        std::this_thread::sleep_until(next);
    }
}

} // namespace target_gen
