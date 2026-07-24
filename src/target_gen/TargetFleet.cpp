#include "TargetFleet.hpp"

#include <chrono>
#include <cmath>
#include <iostream>

#include "PeriodicDeadline.hpp"
#include "SimClock.hpp"

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

TargetFleet::TargetFleet(int32_t domain_id, int num_targets)
    : domain_id_(domain_id),
      participant_(radds::make_participant(
          domain_id, radar::dds_names::PROFILE_TARGETGEN_PARTICIPANT,
          "TargetGen.Generator")),
      publisher_(participant_),
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

    thread_ = std::thread([this] { loop(); });
}

void TargetFleet::stop() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
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

        for (const int32_t id : scenario_.step(kDt, t)) {
            std::cout << "[target_gen] target " << id
                      << " respawned inbound (range > "
                      << scenario_.respawn_range_km() << " km)" << std::endl;
        }

        const double ship_e = scenario_.ship_east_m();
        const double ship_n = scenario_.ship_north_m();
        lat = TargetScenario::kShipStartLat + ship_n / kMetersPerDegLat;
        lon = TargetScenario::kShipStartLon +
              ship_e / (kMetersPerDegLat * std::cos(lat * kDeg2Rad));

        for (const auto& tgt : scenario_.targets()) {

            radar::types::TargetTruth msg;
            msg.target_id = tgt.id;
            msg.timestamp = radar::SimClock::stamp();
            // ship-relative ENU
            msg.position.x_east_m  = tgt.x - ship_e;
            msg.position.y_north_m = tgt.y - ship_n;
            msg.position.z_up_m    = tgt.z;
            msg.velocity.x_east_m  =
                tgt.velocity_x_mps - TargetScenario::kShipSpeedMps *
                std::sin(TargetScenario::kShipHeadingDeg * kDeg2Rad);
            msg.velocity.y_north_m =
                tgt.velocity_y_mps - TargetScenario::kShipSpeedMps *
                std::cos(TargetScenario::kShipHeadingDeg * kDeg2Rad);
            msg.velocity.z_up_m    = 0.0;
            msg.acceleration.x_east_m  = 0.0;
            msg.acceleration.y_north_m = 0.0;
            msg.acceleration.z_up_m    = 0.0;
            msg.rcs_dbsm    = tgt.rcs_dbsm;
            msg.target_type = static_cast<radar::types::TargetType>(tgt.type);
            truth_writer_.write(msg);
        }

        // ship truth at 10 Hz (every 5th cycle)
        if (++cycle % 5 == 0) {
            radar::types::ShipPosition ship;
            ship.source_id     = 1;
            ship.timestamp     = radar::SimClock::stamp();
            ship.latitude_deg  = lat;
            ship.longitude_deg = lon;
            ship.altitude_m    = 0.0;
            ship.heading_deg   = TargetScenario::kShipHeadingDeg;
            ship.course_deg    = TargetScenario::kShipHeadingDeg;
            ship.speed_mps     = TargetScenario::kShipSpeedMps;
            ship.pitch_deg     = 0.8 * std::sin(t * 0.50);
            ship.roll_deg      = 2.5 * std::sin(t * 0.31 + 1.2);
            ship_writer_.write(ship);
        }

        std::this_thread::sleep_until(next);
    }
}

} // namespace target_gen
