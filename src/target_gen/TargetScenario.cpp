#include "TargetScenario.hpp"

#include <algorithm>
#include <cmath>

namespace target_gen {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg2Rad = kPi / 180.0;
constexpr double kGoldenAngleDeg = 137.50776405003785;

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

constexpr int kNumProfiles =
    static_cast<int>(sizeof(kProfiles) / sizeof(kProfiles[0]));

constexpr std::array<ScenarioTemplateInfo, 6> kCatalog{{
    {
        TargetScenario::kOrbitScenario,
        "12 km Orbit",
        "One persistent fighter orbiting the ship at 12 km slant range.",
        false, 1, 1, 1,
    },
    {
        TargetScenario::kRandomFleetScenario,
        "Random Fleet",
        "Configurable randomized inbound targets using the existing profiles.",
        true, 31, 1, 255,
    },
    {
        TargetScenario::kMinimumRangeScenario,
        "Minimum-Range Transit",
        "One-shot flyby: 8 km inbound, 15 s inside 2 km, then 20 km outbound.",
        false, 1, 1, 1,
    },
    {
        TargetScenario::kFaceSeamScenario,
        "Face-Seam Handoff",
        "One fighter arcs across adjacent array faces at 18 km.",
        false, 1, 1, 1,
    },
    {
        TargetScenario::kCrossingPairScenario,
        "Crossing Pair",
        "Two fighters cross with a 500 m horizontal miss, then diverge.",
        false, 2, 2, 2,
    },
    {
        TargetScenario::kFaceBoundaryCrossingPairScenario,
        "Boundary Crossing Pair",
        "Two fighters cross and exchange faces directly on an array seam.",
        false, 2, 2, 2,
    },
}};

double wrap360(double angle_deg) {
    angle_deg = std::fmod(angle_deg, 360.0);
    return angle_deg < 0.0 ? angle_deg + 360.0 : angle_deg;
}

} // namespace

const std::array<ScenarioTemplateInfo, 6>& TargetScenario::catalog() {
    return kCatalog;
}

TargetScenario::TargetScenario(int num_targets, uint64_t seed)
    : rng_(seed) {
    num_targets = std::clamp(num_targets, 1, 256);

    const int64_t orbit_scenario = create_scenario_instance(kOrbitScenario);
    targets_.push_back(
        make_orbit_target(kBaselineTargetId, orbit_scenario, 0.0));

    const int randomized_target_count = num_targets - 1;
    if (randomized_target_count > 0) {
        const int64_t random_scenario =
            create_scenario_instance(kRandomFleetScenario);
        targets_.reserve(static_cast<std::size_t>(num_targets));
        for (int i = 0; i < randomized_target_count; ++i) {
            targets_.push_back(
                make_random_target(100 + i, random_scenario, next_profile_++));
        }
    }
    revision_ = 1;
}

int64_t TargetScenario::create_scenario_instance(
        std::string_view template_name) {
    const int64_t id = next_scenario_id_++;
    scenarios_.push_back({id, std::string(template_name)});
    return id;
}

TargetState TargetScenario::make_orbit_target(
        int32_t target_id, int64_t scenario_id, double phase) {
    const double elevation_rad = kBaselineElevationDeg * kDeg2Rad;
    const double horizontal_radius =
        kBaselineRangeM * std::cos(elevation_rad);

    TargetState target;
    target.id = target_id;
    target.scenario_instance_id = scenario_id;
    target.type = ScenarioTargetType::Fighter;
    target.motion = TargetMotion::Orbit;
    target.phase = phase;
    target.x = ship_e_ + horizontal_radius * std::sin(phase);
    target.y = ship_n_ + horizontal_radius * std::cos(phase);
    target.z = kBaselineRangeM * std::sin(elevation_rad);
    target.speed_mps = kBaselineSpeedMps;
    target.heading_deg = wrap360(90.0 - phase / kDeg2Rad);
    target.rcs_dbsm = 0.0;
    target.profile = -1;
    return target;
}

TargetState TargetScenario::make_random_target(
        int32_t target_id, int64_t scenario_id, int profile_index) {
    std::uniform_real_distribution<double> az_dist(0.0, 360.0);
    std::uniform_real_distribution<double> r_dist(25000.0, 80000.0);
    std::uniform_real_distribution<double> phase_dist(0.0, 2.0 * kPi);

    const int normalized_profile = profile_index % kNumProfiles;
    const auto& profile = kProfiles[normalized_profile];
    const double az = az_dist(rng_) * kDeg2Rad;
    const double range = r_dist(rng_);

    TargetState target;
    target.id = target_id;
    target.scenario_instance_id = scenario_id;
    target.type = profile.type;
    target.motion = TargetMotion::RandomInbound;
    target.x = ship_e_ + range * std::sin(az);
    target.y = ship_n_ + range * std::cos(az);
    target.z = profile.altitude_m;
    target.speed_mps = profile.speed_mps;
    target.heading_deg = wrap360(
        az / kDeg2Rad + 180.0 + (az_dist(rng_) - 180.0) * 0.2);
    target.rcs_dbsm = profile.rcs_dbsm;
    target.maneuver = profile.maneuver;
    target.phase = phase_dist(rng_);
    target.profile = normalized_profile;
    return target;
}

TargetState TargetScenario::make_minimum_range_target(
        int32_t target_id, int64_t scenario_id) {
    // A straight 250 m/s chord with 696 m closest slant approach spends
    // exactly 15 seconds inside a 2 km sphere. Split that closest approach
    // into altitude and lateral miss at 14 degrees so elevation coverage,
    // rather than an overhead pass, cannot explain the blind-range loss.
    const double half_inner_path =
        0.5 * kTransitSpeedMps * kTransitInnerDurationS;
    const double closest_slant_m = std::sqrt(
        kTransitInnerRangeM * kTransitInnerRangeM -
        half_inner_path * half_inner_path);
    const double altitude_m =
        closest_slant_m * std::sin(kBaselineElevationDeg * kDeg2Rad);
    const double cross_track_m =
        closest_slant_m * std::cos(kBaselineElevationDeg * kDeg2Rad);
    const double along_start_m = std::sqrt(
        kTransitStartRangeM * kTransitStartRangeM -
        closest_slant_m * closest_slant_m);
    const double along_end_m = std::sqrt(
        kTransitEndRangeM * kTransitEndRangeM -
        closest_slant_m * closest_slant_m);

    const double bearing_deg = wrap360(
        kShipHeadingDeg +
        static_cast<double>(scenario_id - 1) * kGoldenAngleDeg);
    const double bearing_rad = bearing_deg * kDeg2Rad;
    const double along_e = std::sin(bearing_rad);
    const double along_n = std::cos(bearing_rad);
    const double cross_e = std::cos(bearing_rad);
    const double cross_n = -std::sin(bearing_rad);
    const double ship_velocity_e =
        kShipSpeedMps * std::sin(kShipHeadingDeg * kDeg2Rad);
    const double ship_velocity_n =
        kShipSpeedMps * std::cos(kShipHeadingDeg * kDeg2Rad);

    TargetState target;
    target.id = target_id;
    target.scenario_instance_id = scenario_id;
    target.type = ScenarioTargetType::Fighter;
    target.motion = TargetMotion::MinimumRangeTransit;
    target.x = ship_e_ + along_start_m * along_e +
               cross_track_m * cross_e;
    target.y = ship_n_ + along_start_m * along_n +
               cross_track_m * cross_n;
    target.z = altitude_m;
    target.speed_mps = kTransitSpeedMps;
    target.velocity_x_mps =
        ship_velocity_e - kTransitSpeedMps * along_e;
    target.velocity_y_mps =
        ship_velocity_n - kTransitSpeedMps * along_n;
    target.heading_deg = wrap360(
        std::atan2(target.velocity_x_mps, target.velocity_y_mps) / kDeg2Rad);
    target.rcs_dbsm = 0.0;
    target.profile = -1;
    target.path_length_m = along_start_m + along_end_m;
    return target;
}

TargetState TargetScenario::make_face_seam_target(
        int32_t target_id, int64_t scenario_id, int launch_index) {
    const double elevation_rad = kBaselineElevationDeg * kDeg2Rad;
    const double slant_range_m =
        kFaceSeamRangeM +
        250.0 * static_cast<double>(launch_index / 4);
    const double horizontal_radius =
        slant_range_m * std::cos(elevation_rad);
    const double seam_ship_deg =
        90.0 * static_cast<double>((launch_index % 4) + 1);
    const double start_world_deg =
        kShipHeadingDeg + seam_ship_deg - 0.5 * kFaceSeamArcDeg;
    const double phase = start_world_deg * kDeg2Rad;
    const double ship_velocity_e =
        kShipSpeedMps * std::sin(kShipHeadingDeg * kDeg2Rad);
    const double ship_velocity_n =
        kShipSpeedMps * std::cos(kShipHeadingDeg * kDeg2Rad);
    const double relative_velocity_e =
        kFaceSeamSpeedMps * std::cos(phase);
    const double relative_velocity_n =
        -kFaceSeamSpeedMps * std::sin(phase);

    TargetState target;
    target.id = target_id;
    target.scenario_instance_id = scenario_id;
    target.type = ScenarioTargetType::Fighter;
    target.motion = TargetMotion::FaceSeamHandoff;
    target.phase = phase;
    target.x = ship_e_ + horizontal_radius * std::sin(phase);
    target.y = ship_n_ + horizontal_radius * std::cos(phase);
    target.z = slant_range_m * std::sin(elevation_rad);
    target.speed_mps = kFaceSeamSpeedMps;
    target.velocity_x_mps = ship_velocity_e + relative_velocity_e;
    target.velocity_y_mps = ship_velocity_n + relative_velocity_n;
    target.heading_deg = wrap360(
        std::atan2(
            target.velocity_x_mps,
            target.velocity_y_mps) / kDeg2Rad);
    target.rcs_dbsm = 0.0;
    target.profile = -1;
    target.path_length_m =
        horizontal_radius * kFaceSeamArcDeg * kDeg2Rad;
    return target;
}

std::array<TargetState, 2> TargetScenario::make_crossing_pair(
        int32_t first_target_id,
        int64_t scenario_id,
        double center_ship_bearing_deg,
        double center_slant_range_m) {
    const double center_bearing_deg = wrap360(
        kShipHeadingDeg + center_ship_bearing_deg);
    const double center_bearing_rad = center_bearing_deg * kDeg2Rad;
    const double radial_e = std::sin(center_bearing_rad);
    const double radial_n = std::cos(center_bearing_rad);
    const double tangent_e = std::cos(center_bearing_rad);
    const double tangent_n = -std::sin(center_bearing_rad);
    const double elevation_rad = kBaselineElevationDeg * kDeg2Rad;
    const double center_horizontal_m =
        center_slant_range_m * std::cos(elevation_rad);
    const double center_altitude_m =
        center_slant_range_m * std::sin(elevation_rad);
    const double half_path_m = 0.5 * kCrossingPathLengthM;
    const double inverse_sqrt_two = 1.0 / std::sqrt(2.0);
    const double ship_velocity_e =
        kShipSpeedMps * std::sin(kShipHeadingDeg * kDeg2Rad);
    const double ship_velocity_n =
        kShipSpeedMps * std::cos(kShipHeadingDeg * kDeg2Rad);

    std::array<TargetState, 2> targets;
    for (int i = 0; i < 2; ++i) {
        const double side = i == 0 ? 1.0 : -1.0;
        const double direction_e =
            side * tangent_e * inverse_sqrt_two -
            radial_e * inverse_sqrt_two;
        const double direction_n =
            side * tangent_n * inverse_sqrt_two -
            radial_n * inverse_sqrt_two;
        const double miss_offset_m =
            side * 0.5 * kCrossingHorizontalMissM;

        TargetState& target = targets[static_cast<std::size_t>(i)];
        target.id = first_target_id + i;
        target.scenario_instance_id = scenario_id;
        target.type = ScenarioTargetType::Fighter;
        target.motion = TargetMotion::CrossingPair;
        target.x =
            ship_e_ +
            (center_horizontal_m + miss_offset_m) * radial_e -
            half_path_m * direction_e;
        target.y =
            ship_n_ +
            (center_horizontal_m + miss_offset_m) * radial_n -
            half_path_m * direction_n;
        target.z =
            center_altitude_m +
            side * 0.5 * kCrossingAltitudeSeparationM;
        target.speed_mps = kCrossingSpeedMps;
        target.velocity_x_mps =
            ship_velocity_e + kCrossingSpeedMps * direction_e;
        target.velocity_y_mps =
            ship_velocity_n + kCrossingSpeedMps * direction_n;
        target.heading_deg = wrap360(
            std::atan2(
                target.velocity_x_mps,
                target.velocity_y_mps) / kDeg2Rad);
        target.rcs_dbsm = 0.0;
        target.profile = -1;
        target.path_length_m = kCrossingPathLengthM;
    }
    return targets;
}

ScenarioChange TargetScenario::add_scenario(
        std::string_view template_name, int target_count) {
    const auto descriptor = std::find_if(
        kCatalog.begin(), kCatalog.end(),
        [&](const auto& entry) { return entry.name == template_name; });
    if (descriptor == kCatalog.end())
        return {false, 0, "unknown scenario template"};

    if (!descriptor->configurable_target_count) {
        target_count = descriptor->default_target_count;
    } else if (target_count == 0) {
        target_count = descriptor->default_target_count;
    }
    if (target_count < descriptor->minimum_target_count ||
        target_count > descriptor->maximum_target_count) {
        return {false, 0, "target count is outside the scenario limits"};
    }
    if (targets_.size() + static_cast<std::size_t>(target_count) > 256)
        return {false, 0, "adding the scenario would exceed 256 targets"};

    ScenarioChange change;
    change.accepted = true;
    change.scenario_instance_id =
        create_scenario_instance(template_name);
    change.message = "scenario added";
    change.added_target_ids.reserve(static_cast<std::size_t>(target_count));

    if (template_name == kOrbitScenario) {
        const double phase = std::fmod(
            static_cast<double>(change.scenario_instance_id - 1) *
                kGoldenAngleDeg * kDeg2Rad,
            2.0 * kPi);
        const int32_t id = next_target_id_++;
        targets_.push_back(
            make_orbit_target(id, change.scenario_instance_id, phase));
        change.added_target_ids.push_back(id);
    } else if (template_name == kRandomFleetScenario) {
        for (int i = 0; i < target_count; ++i) {
            const int32_t id = next_target_id_++;
            targets_.push_back(make_random_target(
                id, change.scenario_instance_id, next_profile_++));
            change.added_target_ids.push_back(id);
        }
    } else if (template_name == kMinimumRangeScenario) {
        const int32_t id = next_target_id_++;
        targets_.push_back(make_minimum_range_target(
            id, change.scenario_instance_id));
        change.added_target_ids.push_back(id);
    } else if (template_name == kFaceSeamScenario) {
        const int32_t id = next_target_id_++;
        targets_.push_back(make_face_seam_target(
            id, change.scenario_instance_id,
            next_face_seam_launch_++));
        change.added_target_ids.push_back(id);
    } else if (template_name == kCrossingPairScenario) {
        const int32_t first_id = next_target_id_;
        next_target_id_ += 2;
        const auto pair = make_crossing_pair(
            first_id, change.scenario_instance_id,
            static_cast<double>(next_crossing_pair_launch_++) *
                kGoldenAngleDeg,
            kCrossingCenterRangeM);
        for (const auto& target : pair) {
            targets_.push_back(target);
            change.added_target_ids.push_back(target.id);
        }
    } else {
        const int launch_index =
            next_boundary_crossing_pair_launch_++;
        const double seam_ship_bearing_deg =
            90.0 * static_cast<double>(launch_index % 4);
        const double center_slant_range_m =
            kCrossingCenterRangeM +
            250.0 * static_cast<double>(launch_index / 4);
        const int32_t first_id = next_target_id_;
        next_target_id_ += 2;
        const auto pair = make_crossing_pair(
            first_id, change.scenario_instance_id,
            seam_ship_bearing_deg, center_slant_range_m);
        for (const auto& target : pair) {
            targets_.push_back(target);
            change.added_target_ids.push_back(target.id);
        }
    }

    ++revision_;
    return change;
}

ScenarioChange TargetScenario::remove_scenario(
        int64_t scenario_instance_id) {
    const auto scenario = std::find_if(
        scenarios_.begin(), scenarios_.end(),
        [&](const auto& item) { return item.id == scenario_instance_id; });
    if (scenario == scenarios_.end())
        return {false, scenario_instance_id, "scenario instance not found"};

    ScenarioChange change;
    change.accepted = true;
    change.scenario_instance_id = scenario_instance_id;
    change.message = "scenario removed";
    for (const auto& target : targets_) {
        if (target.scenario_instance_id == scenario_instance_id)
            change.removed_target_ids.push_back(target.id);
    }
    std::erase_if(
        targets_, [&](const auto& target) {
            return target.scenario_instance_id == scenario_instance_id;
        });
    scenarios_.erase(scenario);
    ++revision_;
    return change;
}

ScenarioChange TargetScenario::remove_target(int32_t target_id) {
    const auto target = std::find_if(
        targets_.begin(), targets_.end(),
        [&](const auto& item) { return item.id == target_id; });
    if (target == targets_.end())
        return {false, 0, "target not found"};

    const int64_t scenario_id = target->scenario_instance_id;
    ScenarioChange change;
    change.accepted = true;
    change.scenario_instance_id = scenario_id;
    change.message = "target removed";
    change.removed_target_ids.push_back(target_id);
    targets_.erase(target);
    remove_empty_scenario(scenario_id);
    ++revision_;
    return change;
}

ScenarioChange TargetScenario::clear_all() {
    ScenarioChange change;
    change.accepted = true;
    change.message = "all targets cleared";
    change.removed_target_ids.reserve(targets_.size());
    for (const auto& target : targets_)
        change.removed_target_ids.push_back(target.id);
    targets_.clear();
    scenarios_.clear();
    ++revision_;
    return change;
}

void TargetScenario::remove_empty_scenario(int64_t scenario_instance_id) {
    const bool has_target = std::any_of(
        targets_.begin(), targets_.end(),
        [&](const auto& target) {
            return target.scenario_instance_id == scenario_instance_id;
        });
    if (!has_target) {
        std::erase_if(
            scenarios_, [&](const auto& scenario) {
                return scenario.id == scenario_instance_id;
            });
    }
}

std::size_t TargetScenario::scenario_target_count(
        int64_t scenario_instance_id) const {
    return static_cast<std::size_t>(std::count_if(
        targets_.begin(), targets_.end(),
        [&](const auto& target) {
            return target.scenario_instance_id == scenario_instance_id;
        }));
}

void TargetScenario::respawn(TargetState& target) {
    std::uniform_real_distribution<double> az_dist(0.0, 360.0);
    std::uniform_real_distribution<double> r_dist(25000.0, 80000.0);
    std::uniform_real_distribution<double> phase_dist(0.0, 2.0 * kPi);
    const double az = az_dist(rng_) * kDeg2Rad;
    const double range = r_dist(rng_);

    target.x = ship_e_ + range * std::sin(az);
    target.y = ship_n_ + range * std::cos(az);
    target.z = kProfiles[target.profile].altitude_m;
    target.heading_deg = wrap360(
        az / kDeg2Rad + 180.0 + (az_dist(rng_) - 180.0) * 0.2);
    target.phase = phase_dist(rng_);
}

ScenarioStep TargetScenario::step(double dt, double sim_time_s) {
    const double ship_velocity_east =
        kShipSpeedMps * std::sin(kShipHeadingDeg * kDeg2Rad);
    const double ship_velocity_north =
        kShipSpeedMps * std::cos(kShipHeadingDeg * kDeg2Rad);
    ship_e_ += ship_velocity_east * dt;
    ship_n_ += ship_velocity_north * dt;

    ScenarioStep result;
    for (auto target = targets_.begin(); target != targets_.end();) {
        if (target->motion == TargetMotion::Orbit) {
            const double elevation_rad =
                kBaselineElevationDeg * kDeg2Rad;
            const double horizontal_radius =
                kBaselineRangeM * std::cos(elevation_rad);
            const double angular_rate =
                kBaselineSpeedMps / horizontal_radius;

            target->phase = std::fmod(
                target->phase + angular_rate * dt, 2.0 * kPi);
            const double sin_phase = std::sin(target->phase);
            const double cos_phase = std::cos(target->phase);
            const double relative_velocity_east =
                kBaselineSpeedMps * cos_phase;
            const double relative_velocity_north =
                -kBaselineSpeedMps * sin_phase;

            target->x = ship_e_ + horizontal_radius * sin_phase;
            target->y = ship_n_ + horizontal_radius * cos_phase;
            target->z = kBaselineRangeM * std::sin(elevation_rad);
            target->velocity_x_mps =
                ship_velocity_east + relative_velocity_east;
            target->velocity_y_mps =
                ship_velocity_north + relative_velocity_north;
            target->heading_deg = wrap360(
                std::atan2(relative_velocity_east,
                           relative_velocity_north) / kDeg2Rad);
            ++target;
            continue;
        }

        if (target->motion == TargetMotion::FaceSeamHandoff) {
            const double elevation_rad =
                kBaselineElevationDeg * kDeg2Rad;
            const double slant_range_m =
                target->z / std::sin(elevation_rad);
            const double horizontal_radius =
                slant_range_m * std::cos(elevation_rad);
            const double angular_rate =
                kFaceSeamSpeedMps / horizontal_radius;

            target->phase = std::fmod(
                target->phase + angular_rate * dt, 2.0 * kPi);
            const double sin_phase = std::sin(target->phase);
            const double cos_phase = std::cos(target->phase);
            const double relative_velocity_east =
                kFaceSeamSpeedMps * cos_phase;
            const double relative_velocity_north =
                -kFaceSeamSpeedMps * sin_phase;

            target->x = ship_e_ + horizontal_radius * sin_phase;
            target->y = ship_n_ + horizontal_radius * cos_phase;
            target->velocity_x_mps =
                ship_velocity_east + relative_velocity_east;
            target->velocity_y_mps =
                ship_velocity_north + relative_velocity_north;
            target->heading_deg = wrap360(
                std::atan2(
                    target->velocity_x_mps,
                    target->velocity_y_mps) / kDeg2Rad);
            target->path_progress_m += target->speed_mps * dt;
            if (target->path_progress_m + 1.0e-9 >=
                target->path_length_m) {
                const int32_t target_id = target->id;
                const int64_t scenario_id =
                    target->scenario_instance_id;
                result.removed_target_ids.push_back(target_id);
                target = targets_.erase(target);
                remove_empty_scenario(scenario_id);
                result.completed_scenario_ids.push_back(scenario_id);
                ++revision_;
                continue;
            }
            ++target;
            continue;
        }

        if (target->motion == TargetMotion::MinimumRangeTransit ||
            target->motion == TargetMotion::CrossingPair) {
            target->x += target->velocity_x_mps * dt;
            target->y += target->velocity_y_mps * dt;
            target->path_progress_m += target->speed_mps * dt;
            if (target->path_progress_m + 1.0e-9 >=
                target->path_length_m) {
                const int32_t target_id = target->id;
                const int64_t scenario_id = target->scenario_instance_id;
                result.removed_target_ids.push_back(target_id);
                target = targets_.erase(target);
                if (scenario_target_count(scenario_id) == 0) {
                    remove_empty_scenario(scenario_id);
                    result.completed_scenario_ids.push_back(
                        scenario_id);
                }
                ++revision_;
                continue;
            }
            ++target;
            continue;
        }

        // Keep the live generator's ordering: turn-rate changes take effect
        // on the next step, and published velocity uses this heading.
        const double hx = std::sin(target->heading_deg * kDeg2Rad);
        const double hy = std::cos(target->heading_deg * kDeg2Rad);
        target->velocity_x_mps = target->speed_mps * hx;
        target->velocity_y_mps = target->speed_mps * hy;

        if (target->maneuver == 1) {
            const double weave =
                std::sin(sim_time_s * 0.5 + target->phase) * 0.6;
            const double px = hy;
            const double py = -hx;
            target->x +=
                (target->velocity_x_mps + px * weave * 30.0) * dt;
            target->y +=
                (target->velocity_y_mps + py * weave * 30.0) * dt;
        } else if (target->maneuver == 2) {
            target->heading_deg =
                wrap360(target->heading_deg + 6.0 * dt);
            target->x += target->velocity_x_mps * dt;
            target->y += target->velocity_y_mps * dt;
        } else {
            target->x += target->velocity_x_mps * dt;
            target->y += target->velocity_y_mps * dt;
        }

        if (target->type == ScenarioTargetType::Missile)
            target->z = std::max(200.0, target->z - 30.0 * dt);

        if (respawn_range_m_ > 0.0 &&
            std::hypot(target->x - ship_e_, target->y - ship_n_) >
                respawn_range_m_) {
            respawn(*target);
            result.respawned_target_ids.push_back(target->id);
        }
        ++target;
    }
    return result;
}

} // namespace target_gen
