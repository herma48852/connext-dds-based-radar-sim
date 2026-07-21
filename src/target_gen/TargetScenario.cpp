#include "TargetScenario.hpp"

#include <algorithm>
#include <cmath>

namespace target_gen {
namespace {

constexpr double kDeg2Rad = 3.14159265358979323846 / 180.0;

struct Profile {
    ScenarioTargetType type;
    double speed_mps;
    double altitude_m;
    double rcs_dbsm;
    int maneuver;
};

constexpr Profile kProfiles[] = {
    {ScenarioTargetType::Fighter, 250.0,  8000.0,   0.0, 1},
    {ScenarioTargetType::Fighter, 280.0,  9000.0,   0.0, 0},
    {ScenarioTargetType::Bomber,  200.0, 10000.0,  20.0, 0},
    {ScenarioTargetType::Missile, 600.0, 12000.0, -10.0, 0},
    {ScenarioTargetType::Missile, 650.0, 11000.0, -10.0, 0},
    {ScenarioTargetType::Ship,     12.0,     0.0,  35.0, 0},
    {ScenarioTargetType::Drone,    60.0,  1500.0, -15.0, 2},
    {ScenarioTargetType::Decoy,   240.0,  7500.0,   5.0, 1},
};

constexpr int kNumProfiles = static_cast<int>(sizeof(kProfiles) / sizeof(kProfiles[0]));

} // namespace

TargetScenario::TargetScenario(int num_targets, uint64_t seed)
    : rng_(seed) {
    std::uniform_real_distribution<double> az_dist(0.0, 360.0);
    std::uniform_real_distribution<double> r_dist(25000.0, 80000.0);
    std::uniform_real_distribution<double> phase_dist(0.0, 6.2832);

    if (num_targets < 0) num_targets = 0;
    targets_.reserve(static_cast<size_t>(num_targets));
    for (int i = 0; i < num_targets; ++i) {
        const auto& profile = kProfiles[i % kNumProfiles];
        const double az = az_dist(rng_) * kDeg2Rad;
        const double range = r_dist(rng_);

        TargetState target;
        target.id = 100 + i;
        target.type = profile.type;
        target.x = range * std::sin(az);
        target.y = range * std::cos(az);
        target.z = profile.altitude_m;
        target.speed_mps = profile.speed_mps;
        target.heading_deg = std::fmod(
            az / kDeg2Rad + 180.0 + (az_dist(rng_) - 180.0) * 0.2,
            360.0);
        target.rcs_dbsm = profile.rcs_dbsm;
        target.maneuver = profile.maneuver;
        target.phase = phase_dist(rng_);
        target.profile = i % kNumProfiles;
        targets_.push_back(target);
    }
}

void TargetScenario::respawn(TargetState& target) {
    std::uniform_real_distribution<double> az_dist(0.0, 360.0);
    std::uniform_real_distribution<double> r_dist(25000.0, 80000.0);
    std::uniform_real_distribution<double> phase_dist(0.0, 6.2832);
    const double az = az_dist(rng_) * kDeg2Rad;
    const double range = r_dist(rng_);

    target.x = ship_e_ + range * std::sin(az);
    target.y = ship_n_ + range * std::cos(az);
    target.z = kProfiles[target.profile].altitude_m;
    target.heading_deg = std::fmod(
        az / kDeg2Rad + 180.0 + (az_dist(rng_) - 180.0) * 0.2,
        360.0);
    target.phase = phase_dist(rng_);
}

std::vector<int32_t> TargetScenario::step(double dt, double sim_time_s) {
    ship_e_ += kShipSpeedMps * std::sin(kShipHeadingDeg * kDeg2Rad) * dt;
    ship_n_ += kShipSpeedMps * std::cos(kShipHeadingDeg * kDeg2Rad) * dt;

    std::vector<int32_t> respawned;
    for (auto& target : targets_) {
        // Keep the live generator's ordering: orbit heading changes take
        // effect on the next step, and published velocity uses this heading.
        const double hx = std::sin(target.heading_deg * kDeg2Rad);
        const double hy = std::cos(target.heading_deg * kDeg2Rad);
        target.velocity_x_mps = target.speed_mps * hx;
        target.velocity_y_mps = target.speed_mps * hy;

        if (target.maneuver == 1) {
            const double weave = std::sin(sim_time_s * 0.5 + target.phase) * 0.6;
            const double px = hy;
            const double py = -hx;
            target.x += (target.velocity_x_mps + px * weave * 30.0) * dt;
            target.y += (target.velocity_y_mps + py * weave * 30.0) * dt;
        } else if (target.maneuver == 2) {
            target.heading_deg = std::fmod(target.heading_deg + 6.0 * dt, 360.0);
            target.x += target.velocity_x_mps * dt;
            target.y += target.velocity_y_mps * dt;
        } else {
            target.x += target.velocity_x_mps * dt;
            target.y += target.velocity_y_mps * dt;
        }

        if (target.type == ScenarioTargetType::Missile)
            target.z = std::max(200.0, target.z - 30.0 * dt);

        if (respawn_range_m_ > 0.0 &&
            std::hypot(target.x - ship_e_, target.y - ship_n_) > respawn_range_m_) {
            respawn(target);
            respawned.push_back(target.id);
        }
    }
    return respawned;
}

} // namespace target_gen
