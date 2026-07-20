#include "TargetFleet.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <random>

#include "SimClock.hpp"

namespace target_gen {

// File-scope alias so both the anonymous namespace below and loop() can use it.
using TT = radar::types::TargetType;

namespace {
constexpr double kDeg2Rad = 3.14159265358979323846 / 180.0;

struct Profile {
    int32_t type;
    double  speed_mps;
    double  altitude_m;
    double  rcs_dbsm;
    int     maneuver;
};

// A plausible mix of traffic for the demo
const Profile kProfiles[] = {
    {static_cast<int32_t>(TT::TARGET_FIGHTER), 250.0,  8000.0,   0.0, 1}, // weaving fighter
    {static_cast<int32_t>(TT::TARGET_FIGHTER), 280.0,  9000.0,   0.0, 0},
    {static_cast<int32_t>(TT::TARGET_BOMBER),  200.0, 10000.0,  20.0, 0},
    {static_cast<int32_t>(TT::TARGET_MISSILE), 600.0, 12000.0, -10.0, 0}, // fast inbounds
    {static_cast<int32_t>(TT::TARGET_MISSILE), 650.0, 11000.0, -10.0, 0},
    {static_cast<int32_t>(TT::TARGET_SHIP),     12.0,     0.0,  35.0, 0},
    {static_cast<int32_t>(TT::TARGET_DRONE),    60.0,  1500.0, -15.0, 2}, // orbiting drone
    {static_cast<int32_t>(TT::TARGET_DECOY),   240.0,  7500.0,   5.0, 1},
};
constexpr int kNumProfiles = sizeof(kProfiles) / sizeof(kProfiles[0]);
} // namespace

TargetFleet::TargetFleet(int32_t domain_id, int num_targets)
    : domain_id_(domain_id),
      participant_(radds::make_participant(
          domain_id, radar::dds_names::PROFILE_TARGETGEN_PARTICIPANT,
          "TargetGen.Generator")),
      publisher_(participant_) {
    // rng_ (member, seeded 20260719): deterministic scenario for the
    // webinar; also drives respawns so runs stay reproducible.
    std::uniform_real_distribution<double> az_dist(0.0, 360.0);
    std::uniform_real_distribution<double> r_dist(25000.0, 80000.0);
    std::uniform_real_distribution<double> phase_dist(0.0, 6.2832);

    for (int i = 0; i < num_targets; ++i) {
        const auto& p = kProfiles[i % kNumProfiles];
        const double az = az_dist(rng_) * kDeg2Rad;
        const double r  = r_dist(rng_);
        Target t{};
        t.id       = 100 + i;
        t.type     = p.type;
        t.x        = r * std::sin(az);
        t.y        = r * std::cos(az);
        t.z        = p.altitude_m;
        t.speed_mps= p.speed_mps;
        // mostly inbound headings (toward the ship) with some spread
        t.heading_deg = std::fmod(az / kDeg2Rad + 180.0 + (az_dist(rng_) - 180.0) * 0.2, 360.0);
        t.rcs_dbsm = p.rcs_dbsm;
        t.maneuver = p.maneuver;
        t.phase    = phase_dist(rng_);
        t.profile  = i % kNumProfiles;
        targets_.push_back(t);
    }
}

void TargetFleet::respawn(Target& t, double ship_e, double ship_n) {
    // Same distributions as the initial layout, but anchored at the
    // ship's current position so the scenario never drifts off it.
    std::uniform_real_distribution<double> az_dist(0.0, 360.0);
    std::uniform_real_distribution<double> r_dist(25000.0, 80000.0);
    std::uniform_real_distribution<double> phase_dist(0.0, 6.2832);
    const double az = az_dist(rng_) * kDeg2Rad;
    const double r  = r_dist(rng_);
    t.x = ship_e + r * std::sin(az);
    t.y = ship_n + r * std::cos(az);
    t.z = kProfiles[t.profile].altitude_m; // missiles dive; reset
    t.heading_deg = std::fmod(az / kDeg2Rad + 180.0 + (az_dist(rng_) - 180.0) * 0.2, 360.0);
    t.phase = phase_dist(rng_);
}

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

    double ship_e = 0.0, ship_n = 0.0;      // world ENU of the ship [m]
    double lat = kShipStartLat, lon = kShipStartLon;
    auto next = steady_clock::now();
    int cycle = 0;

    while (!stop_.load()) {
        next += milliseconds(20);
        const double t = radar::SimClock::sim_millis() / 1000.0;

        // ship ground truth motion
        ship_e += kShipSpeedMps * std::sin(kShipHeadingDeg * kDeg2Rad) * kDt;
        ship_n += kShipSpeedMps * std::cos(kShipHeadingDeg * kDeg2Rad) * kDt;
        lat = kShipStartLat + ship_n / kMetersPerDegLat;
        lon = kShipStartLon + ship_e / (kMetersPerDegLat * std::cos(lat * kDeg2Rad));

        for (auto& tgt : targets_) {
            // kinematics
            double hx = std::sin(tgt.heading_deg * kDeg2Rad);
            double hy = std::cos(tgt.heading_deg * kDeg2Rad);
            if (tgt.maneuver == 1) {
                // weave: lateral sinusoid
                const double w = std::sin(t * 0.5 + tgt.phase) * 0.6;
                const double px = hy, py = -hx; // perpendicular
                tgt.x += (tgt.speed_mps * hx + px * w * 30.0) * kDt;
                tgt.y += (tgt.speed_mps * hy + py * w * 30.0) * kDt;
            } else if (tgt.maneuver == 2) {
                // slow orbit around current center
                tgt.heading_deg = std::fmod(tgt.heading_deg + 6.0 * kDt, 360.0);
                tgt.x += tgt.speed_mps * hx * kDt;
                tgt.y += tgt.speed_mps * hy * kDt;
            } else {
                tgt.x += tgt.speed_mps * hx * kDt;
                tgt.y += tgt.speed_mps * hy * kDt;
            }
            if (tgt.type == static_cast<int32_t>(TT::TARGET_MISSILE))
                tgt.z = std::max(200.0, tgt.z - 30.0 * kDt); // diving profile

            // Recycle targets that have left coverage for good (they fly
            // straight forever otherwise; the table thins out ~10 min in).
            if (respawn_range_m_ > 0.0 &&
                std::hypot(tgt.x - ship_e, tgt.y - ship_n) > respawn_range_m_) {
                std::cout << "[target_gen] target " << tgt.id
                          << " respawned inbound (range > "
                          << respawn_range_m_ / 1000.0 << " km)" << std::endl;
                respawn(tgt, ship_e, ship_n);
            }

            radar::types::TargetTruth msg;
            msg.target_id = tgt.id;
            msg.timestamp = radar::SimClock::stamp();
            // ship-relative ENU
            msg.position.x_east_m  = tgt.x - ship_e;
            msg.position.y_north_m = tgt.y - ship_n;
            msg.position.z_up_m    = tgt.z;
            msg.velocity.x_east_m  =
                tgt.speed_mps * hx - kShipSpeedMps * std::sin(kShipHeadingDeg * kDeg2Rad);
            msg.velocity.y_north_m =
                tgt.speed_mps * hy - kShipSpeedMps * std::cos(kShipHeadingDeg * kDeg2Rad);
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
            ship.heading_deg   = kShipHeadingDeg;
            ship.course_deg    = kShipHeadingDeg;
            ship.speed_mps     = kShipSpeedMps;
            ship.pitch_deg     = 0.8 * std::sin(t * 0.50);
            ship.roll_deg      = 2.5 * std::sin(t * 0.31 + 1.2);
            ship_writer_.write(ship);
        }

        std::this_thread::sleep_until(next);
    }
}

} // namespace target_gen
