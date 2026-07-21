#pragma once

#include <cstdint>
#include <random>
#include <vector>

namespace target_gen {

// DDS-free target motion core shared by the live generator and regression
// tests. The numeric values intentionally match radar::types::TargetType.
enum class ScenarioTargetType : int32_t {
    Fighter = 0,
    Bomber  = 1,
    Missile = 2,
    Ship    = 3,
    Drone   = 4,
    Decoy   = 5,
};

struct TargetState {
    int32_t id = 0;
    ScenarioTargetType type = ScenarioTargetType::Fighter;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double speed_mps = 0.0;
    double heading_deg = 0.0;
    double rcs_dbsm = 0.0;
    int maneuver = 0;
    double phase = 0.0;
    int profile = 0;
    double velocity_x_mps = 0.0;
    double velocity_y_mps = 0.0;
};

class TargetScenario {
public:
    static constexpr double kShipHeadingDeg = 45.0;
    static constexpr double kShipSpeedMps   = 10.3;
    static constexpr double kShipStartLat   = 36.90;
    static constexpr double kShipStartLon   = -75.90;

    explicit TargetScenario(int num_targets, uint64_t seed = 20260719);

    void set_respawn_range_km(double km) { respawn_range_m_ = km * 1000.0; }
    double respawn_range_km() const { return respawn_range_m_ / 1000.0; }

    // Advance one fixed-duration simulation step. Returned IDs were respawned
    // during this step, which lets the live adapter log and tests assert it.
    std::vector<int32_t> step(double dt, double sim_time_s);

    const std::vector<TargetState>& targets() const { return targets_; }
    double ship_east_m() const { return ship_e_; }
    double ship_north_m() const { return ship_n_; }

private:
    void respawn(TargetState& target);

    std::vector<TargetState> targets_;
    std::mt19937_64 rng_;
    double respawn_range_m_ = 120000.0;
    double ship_e_ = 0.0;
    double ship_n_ = 0.0;
};

} // namespace target_gen
