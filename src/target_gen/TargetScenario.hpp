#pragma once

#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
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

enum class TargetMotion {
    Orbit,
    RandomInbound,
    MinimumRangeTransit,
    FaceSeamHandoff,
    CrossingPair,
};

struct TargetState {
    int32_t id = 0;
    int64_t scenario_instance_id = 0;
    ScenarioTargetType type = ScenarioTargetType::Fighter;
    TargetMotion motion = TargetMotion::RandomInbound;
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
    double path_progress_m = 0.0;
    double path_length_m = 0.0;
    // Non-zero only for persistent orbit scenarios. Keeping the radius in
    // state lets presentation contacts use type-appropriate ranges while
    // sharing the same deterministic orbit integrator as the baseline.
    double orbit_radius_m = 0.0;
};

struct ScenarioTemplateInfo {
    std::string_view name;
    std::string_view label;
    std::string_view description;
    bool configurable_target_count;
    int default_target_count;
    int minimum_target_count;
    int maximum_target_count;
};

struct ScenarioInstance {
    int64_t id = 0;
    std::string template_name;
};

struct ScenarioChange {
    bool accepted = false;
    int64_t scenario_instance_id = 0;
    std::string message;
    std::vector<int32_t> added_target_ids;
    std::vector<int32_t> removed_target_ids;
};

struct ScenarioStep {
    std::vector<int32_t> respawned_target_ids;
    std::vector<int32_t> removed_target_ids;
    std::vector<int64_t> completed_scenario_ids;
};

class TargetScenario {
public:
    static constexpr double kShipHeadingDeg = 45.0;
    static constexpr double kShipSpeedMps   = 10.3;
    static constexpr double kShipStartLat   = 36.90;
    static constexpr double kShipStartLon   = -75.90;
    static constexpr int32_t kBaselineTargetId = 1;
    static constexpr double kBaselineRangeM = 12000.0;
    static constexpr double kBaselineElevationDeg = 14.0;
    static constexpr double kBaselineSpeedMps = 250.0;

    static constexpr std::string_view kOrbitScenario = "orbit_12km";
    static constexpr std::string_view kRandomFleetScenario = "random_fleet";
    static constexpr std::string_view kPresentationFleetScenario =
        "presentation_fleet";
    static constexpr std::string_view kMinimumRangeScenario =
        "minimum_range_transit";
    static constexpr std::string_view kFaceSeamScenario =
        "face_seam_handoff";
    static constexpr std::string_view kCrossingPairScenario =
        "crossing_pair";
    static constexpr std::string_view kFaceBoundaryCrossingPairScenario =
        "face_boundary_crossing_pair";

    static constexpr double kTransitStartRangeM = 8000.0;
    static constexpr double kTransitEndRangeM = 20000.0;
    static constexpr double kTransitInnerRangeM = 2000.0;
    static constexpr double kTransitInnerDurationS = 15.0;
    static constexpr double kTransitSpeedMps = 250.0;

    static constexpr double kFaceSeamRangeM = 18000.0;
    static constexpr double kFaceSeamArcDeg = 24.0;
    static constexpr double kFaceSeamSpeedMps = 250.0;

    static constexpr double kCrossingCenterRangeM = 18000.0;
    static constexpr double kCrossingPathLengthM = 20000.0;
    static constexpr double kCrossingHorizontalMissM = 500.0;
    static constexpr double kCrossingAltitudeSeparationM = 300.0;
    static constexpr double kCrossingSpeedMps = 250.0;

    // Preserve the existing launch behavior as two independent groups:
    // one 12 km orbit plus num_targets - 1 randomized inbound targets.
    explicit TargetScenario(int num_targets, uint64_t seed = 20260719);

    static const std::array<ScenarioTemplateInfo, 7>& catalog();

    void set_respawn_range_km(double km) { respawn_range_m_ = km * 1000.0; }
    double respawn_range_km() const { return respawn_range_m_ / 1000.0; }

    ScenarioChange add_scenario(std::string_view template_name,
                                int target_count = 0);
    ScenarioChange remove_scenario(int64_t scenario_instance_id);
    ScenarioChange remove_target(int32_t target_id);
    ScenarioChange clear_all();

    // Advance one fixed-duration simulation step. One-shot scenarios return
    // the IDs that must be disposed by the live DDS adapter.
    ScenarioStep step(double dt, double sim_time_s);

    const std::vector<TargetState>& targets() const { return targets_; }
    const std::vector<ScenarioInstance>& scenarios() const {
        return scenarios_;
    }
    std::size_t scenario_target_count(int64_t scenario_instance_id) const;
    int64_t revision() const { return revision_; }
    double ship_east_m() const { return ship_e_; }
    double ship_north_m() const { return ship_n_; }

private:
    int64_t create_scenario_instance(std::string_view template_name);
    TargetState make_orbit_target(int32_t target_id, int64_t scenario_id,
                                  double phase);
    TargetState make_random_target(int32_t target_id, int64_t scenario_id,
                                   int profile);
    TargetState make_presentation_target(
        int32_t target_id, int64_t scenario_id, int launch_index);
    TargetState make_minimum_range_target(
        int32_t target_id, int64_t scenario_id);
    TargetState make_face_seam_target(
        int32_t target_id, int64_t scenario_id, int launch_index);
    std::array<TargetState, 2> make_crossing_pair(
        int32_t first_target_id,
        int64_t scenario_id,
        double center_ship_bearing_deg,
        double center_slant_range_m);
    void respawn(TargetState& target);
    void remove_empty_scenario(int64_t scenario_instance_id);

    std::vector<TargetState> targets_;
    std::vector<ScenarioInstance> scenarios_;
    std::mt19937_64 rng_;
    int32_t next_target_id_ = 10000;
    int64_t next_scenario_id_ = 1;
    int next_profile_ = 0;
    int next_presentation_launch_ = 0;
    int next_face_seam_launch_ = 0;
    int next_crossing_pair_launch_ = 0;
    int next_boundary_crossing_pair_launch_ = 0;
    int64_t revision_ = 0;
    double respawn_range_m_ = 120000.0;
    double ship_e_ = 0.0;
    double ship_n_ = 0.0;
};

} // namespace target_gen
