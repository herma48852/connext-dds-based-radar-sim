#include "TargetScenario.hpp"
#include "DetectionModel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <set>
#include <string>

namespace {

using target_gen::ScenarioTargetType;
using target_gen::TargetMotion;
using target_gen::TargetScenario;
using target_gen::TargetState;

bool require(bool condition, const std::string& message) {
    if (!condition)
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
    return condition;
}

bool same_state(const TargetState& a, const TargetState& b) {
    return a.id == b.id &&
           a.scenario_instance_id == b.scenario_instance_id &&
           a.type == b.type && a.motion == b.motion &&
           a.x == b.x && a.y == b.y && a.z == b.z &&
           a.speed_mps == b.speed_mps &&
           a.heading_deg == b.heading_deg &&
           a.rcs_dbsm == b.rcs_dbsm &&
           a.maneuver == b.maneuver && a.phase == b.phase &&
           a.profile == b.profile &&
           a.velocity_x_mps == b.velocity_x_mps &&
           a.velocity_y_mps == b.velocity_y_mps &&
           a.path_progress_m == b.path_progress_m &&
           a.path_length_m == b.path_length_m &&
           a.orbit_radius_m == b.orbit_radius_m;
}

double slant_range(const TargetScenario& scenario,
                   const TargetState& target) {
    return std::hypot(
        std::hypot(
            target.x - scenario.ship_east_m(),
            target.y - scenario.ship_north_m()),
        target.z);
}

double ship_relative_azimuth_deg(
        const TargetScenario& scenario,
        const TargetState& target) {
    double azimuth =
        std::atan2(
            target.x - scenario.ship_east_m(),
            target.y - scenario.ship_north_m()) *
        180.0 / 3.14159265358979323846 -
        TargetScenario::kShipHeadingDeg;
    while (azimuth >= 360.0)
        azimuth -= 360.0;
    while (azimuth < 0.0)
        azimuth += 360.0;
    return azimuth;
}

} // namespace

int main() {
    constexpr int kRegressionTargets = 16;
    constexpr double kDt = 0.02;
    constexpr int kSteps = 90'000; // 30 live minutes, accelerated
    bool ok = true;

    TargetScenario scenario(kRegressionTargets);
    TargetScenario repeat(kRegressionTargets);
    TargetScenario baseline_only(1);

    ok &= require(
        TargetScenario::catalog().size() == 7,
        "the control catalog must advertise all seven scenarios");
    const auto presentation_descriptor = std::find_if(
        TargetScenario::catalog().begin(),
        TargetScenario::catalog().end(),
        [](const auto& entry) {
            return entry.name ==
                TargetScenario::kPresentationFleetScenario;
        });
    ok &= require(
        presentation_descriptor != TargetScenario::catalog().end() &&
            presentation_descriptor->default_target_count == 6 &&
            presentation_descriptor->minimum_target_count == 1 &&
            presentation_descriptor->maximum_target_count == 64,
        "presentation fleet must advertise a configurable six-target default");
    ok &= require(
        baseline_only.targets().size() == 1 &&
            baseline_only.targets().front().motion == TargetMotion::Orbit &&
            baseline_only.scenarios().size() == 1,
        "a one-target launch must contain only the orbit scenario");
    ok &= require(
        scenario.targets().size() == kRegressionTargets &&
            scenario.scenarios().size() == 2,
        "legacy startup must remain orbit plus N-1 random targets");
    ok &= require(
        scenario.respawn_range_km() == 120.0,
        "default periodic respawn range must remain 120 km");

    const auto& baseline = scenario.targets().front();
    const double baseline_horizontal_range =
        std::hypot(baseline.x, baseline.y);
    ok &= require(
        baseline.id == TargetScenario::kBaselineTargetId &&
            baseline.motion == TargetMotion::Orbit &&
            baseline.type == ScenarioTargetType::Fighter &&
            std::abs(slant_range(scenario, baseline) -
                     TargetScenario::kBaselineRangeM) < 1.0e-6 &&
            std::abs(
                std::atan2(baseline.z, baseline_horizontal_range) *
                    180.0 / 3.14159265358979323846 -
                TargetScenario::kBaselineElevationDeg) < 1.0e-12,
        "baseline must start at 12 km on the 14-degree elevation bar");

    const std::array<int, 6> expected_types{5, 2, 4, 2, 2, 1};
    std::array<int, 6> observed_types{};
    std::set<int32_t> ids;
    for (std::size_t i = 0; i < scenario.targets().size(); ++i) {
        const auto& target = scenario.targets()[i];
        ids.insert(target.id);
        ++observed_types[static_cast<std::size_t>(target.type)];
        if (i == 0) {
            ok &= require(
                target.profile == -1,
                "orbit must remain outside the randomized profiles");
        } else {
            const auto random_index = static_cast<int32_t>(i - 1);
            ok &= require(
                target.id == 100 + random_index,
                "startup random IDs must remain stable at 100..114");
            ok &= require(
                target.profile == static_cast<int>(random_index % 8),
                "the eight-profile startup sequence must remain stable");
        }
        ok &= require(
            same_state(target, repeat.targets()[i]),
            "fixed seed must reproduce the initial target picture");
    }
    ok &= require(
        ids.size() == kRegressionTargets,
        "startup target IDs must be unique");
    ok &= require(
        observed_types == expected_types,
        "startup target type mix must remain unchanged");

    // Presentation Fleet is intentionally separate from Random Fleet: one
    // persistent contact per target type, placed at an RCS-appropriate range
    // and a deterministic search-bar geometry.
    TargetScenario presentation(1);
    presentation.clear_all();
    const auto presentation_added = presentation.add_scenario(
        TargetScenario::kPresentationFleetScenario);
    const std::array<ScenarioTargetType, 6> presentation_types{
        ScenarioTargetType::Fighter,
        ScenarioTargetType::Bomber,
        ScenarioTargetType::Missile,
        ScenarioTargetType::Ship,
        ScenarioTargetType::Drone,
        ScenarioTargetType::Decoy};
    const std::array<double, 6> presentation_ranges_m{
        18000.0, 50000.0, 12000.0, 45000.0, 9000.0, 30000.0};
    const std::array<double, 6> presentation_elevations_deg{
        14.0, 14.0, 25.0, 0.0, 14.0, 14.0};
    const std::array<double, 6> presentation_rcs_dbsm{
        0.0, 20.0, -10.0, 35.0, -15.0, 5.0};
    const std::array<double, 6> presentation_speeds_mps{
        250.0, 200.0, 600.0, 12.0, 60.0, 240.0};
    ok &= require(
        presentation_added.accepted &&
            presentation_added.added_target_ids.size() == 6 &&
            presentation.targets().size() == 6 &&
            presentation.scenario_target_count(
                presentation_added.scenario_instance_id) == 6,
        "presentation fleet default must add six persistent contacts");
    for (std::size_t i = 0; i < presentation.targets().size(); ++i) {
        const auto& target = presentation.targets()[i];
        const double range = slant_range(presentation, target);
        const double horizontal_range = std::hypot(
            target.x - presentation.ship_east_m(),
            target.y - presentation.ship_north_m());
        const double elevation_deg =
            std::atan2(target.z, horizontal_range) *
            180.0 / 3.14159265358979323846;
        ok &= require(
            target.type == presentation_types[i] &&
                target.motion == TargetMotion::Orbit &&
                std::abs(range - presentation_ranges_m[i]) < 1.0e-6 &&
                std::abs(elevation_deg -
                         presentation_elevations_deg[i]) < 1.0e-12 &&
                target.rcs_dbsm == presentation_rcs_dbsm[i] &&
                target.speed_mps == presentation_speeds_mps[i] &&
                target.orbit_radius_m > 0.0,
            "presentation contact must retain its type-specific observable "
            "orbit");

        // Require margin even at the nominal 3 dB azimuth-beam edge. This
        // validates presentation placement against the production receiver
        // equation without changing CFAR or target RCS.
        const double beam_edge_response = std::sqrt(0.5);
        const double signal =
            radar::app::detection_model::target_voltage_amplitude(
                target.rcs_dbsm, range, 1.0, beam_edge_response);
        const double integrated_magnitude = std::hypot(
            signal,
            radar::app::detection_model::kNoiseMagnitudeRms);
        ok &= require(
            integrated_magnitude >
                radar::app::detection_model::kCfarThreshold,
            "presentation contact must clear CFAR at the nominal 3 dB "
            "beam edge");
    }
    for (std::size_t i = 0; i < presentation.targets().size(); ++i) {
        for (std::size_t j = i + 1;
             j < presentation.targets().size(); ++j) {
            ok &= require(
                std::hypot(
                    presentation.targets()[i].x -
                        presentation.targets()[j].x,
                    presentation.targets()[i].y -
                        presentation.targets()[j].y) > 1000.0,
                "presentation contacts must begin in distinct resolution "
                "regions");
        }
    }
    for (int step = 0; step < 6'000; ++step) {
        const auto update = presentation.step(kDt, step * kDt);
        ok &= require(
            update.removed_target_ids.empty() &&
                update.respawned_target_ids.empty() &&
                presentation.targets().size() == 6,
            "presentation fleet must persist without recycling or disposal");
    }
    for (std::size_t i = 0; i < presentation.targets().size(); ++i) {
        ok &= require(
            std::abs(
                slant_range(presentation, presentation.targets()[i]) -
                presentation_ranges_m[i]) < 1.0e-6,
            "presentation orbit must preserve its configured slant range");
    }
    const auto repeated_presentation = presentation.add_scenario(
        TargetScenario::kPresentationFleetScenario);
    ok &= require(
        repeated_presentation.accepted &&
            repeated_presentation.scenario_instance_id !=
                presentation_added.scenario_instance_id &&
            presentation.targets().size() == 12 &&
            std::hypot(
                presentation.targets()[0].x -
                    presentation.targets()[6].x,
                presentation.targets()[0].y -
                    presentation.targets()[6].y) > 1000.0,
        "repeated presentation fleets must use separated orbit phases");

    // Additive runtime scenario behavior and deterministic non-overlap.
    TargetScenario additive(1);
    const std::size_t initial_count = additive.targets().size();
    const auto unknown = additive.add_scenario("not_a_scenario");
    ok &= require(
        !unknown.accepted && additive.targets().size() == initial_count,
        "unknown templates must not mutate the fleet");

    const auto orbit_a =
        additive.add_scenario(TargetScenario::kOrbitScenario);
    const auto orbit_b =
        additive.add_scenario(TargetScenario::kOrbitScenario);
    ok &= require(
        orbit_a.accepted && orbit_b.accepted &&
            orbit_a.scenario_instance_id != orbit_b.scenario_instance_id &&
            additive.targets().size() == initial_count + 2,
        "repeated orbit selections must add independent instances");
    const auto& added_orbit_a =
        additive.targets()[additive.targets().size() - 2];
    const auto& added_orbit_b = additive.targets().back();
    ok &= require(
        std::abs(slant_range(additive, added_orbit_a) -
                 TargetScenario::kBaselineRangeM) < 1.0e-6 &&
            std::abs(slant_range(additive, added_orbit_b) -
                     TargetScenario::kBaselineRangeM) < 1.0e-6 &&
            std::hypot(
                added_orbit_a.x - added_orbit_b.x,
                added_orbit_a.y - added_orbit_b.y) > 1000.0,
        "repeated orbit targets must share the ring without overlapping");

    const auto random =
        additive.add_scenario(TargetScenario::kRandomFleetScenario, 4);
    ok &= require(
        random.accepted &&
            additive.scenario_target_count(
                random.scenario_instance_id) == 4,
        "random-fleet selection must add its requested target count");
    const auto transit_a =
        additive.add_scenario(TargetScenario::kMinimumRangeScenario);
    const auto transit_b =
        additive.add_scenario(TargetScenario::kMinimumRangeScenario);
    const auto& transit_target_a =
        additive.targets()[additive.targets().size() - 2];
    const auto& transit_target_b = additive.targets().back();
    ok &= require(
        transit_a.accepted && transit_b.accepted &&
            std::abs(slant_range(additive, transit_target_a) -
                     TargetScenario::kTransitStartRangeM) < 1.0e-6 &&
            std::abs(slant_range(additive, transit_target_b) -
                     TargetScenario::kTransitStartRangeM) < 1.0e-6 &&
            std::hypot(
                transit_target_a.x - transit_target_b.x,
                transit_target_a.y - transit_target_b.y) > 1000.0,
        "repeated transit launches must rotate to distinct bearings");

    const auto handoff_a =
        additive.add_scenario(TargetScenario::kFaceSeamScenario);
    const auto handoff_b =
        additive.add_scenario(TargetScenario::kFaceSeamScenario);
    const auto& handoff_target_a =
        additive.targets()[additive.targets().size() - 2];
    const auto& handoff_target_b = additive.targets().back();
    ok &= require(
        handoff_a.accepted && handoff_b.accepted &&
            handoff_a.added_target_ids.size() == 1 &&
            handoff_b.added_target_ids.size() == 1 &&
            handoff_target_a.motion ==
                TargetMotion::FaceSeamHandoff &&
            handoff_target_b.motion ==
                TargetMotion::FaceSeamHandoff &&
            std::hypot(
                handoff_target_a.x - handoff_target_b.x,
                handoff_target_a.y - handoff_target_b.y) > 1000.0,
        "repeated face-seam handoffs must use different boundaries");

    const auto crossing_a =
        additive.add_scenario(TargetScenario::kCrossingPairScenario);
    const auto crossing_b =
        additive.add_scenario(TargetScenario::kCrossingPairScenario);
    ok &= require(
        crossing_a.accepted && crossing_b.accepted &&
            crossing_a.added_target_ids.size() == 2 &&
            crossing_b.added_target_ids.size() == 2 &&
            additive.scenario_target_count(
                crossing_a.scenario_instance_id) == 2 &&
            additive.scenario_target_count(
                crossing_b.scenario_instance_id) == 2,
        "each crossing-pair selection must add two targets");

    const auto boundary_pair_a = additive.add_scenario(
        TargetScenario::kFaceBoundaryCrossingPairScenario);
    const auto boundary_pair_b = additive.add_scenario(
        TargetScenario::kFaceBoundaryCrossingPairScenario);
    const std::size_t boundary_start =
        additive.targets().size() - 4;
    const auto& boundary_a_first =
        additive.targets()[boundary_start];
    const auto& boundary_a_second =
        additive.targets()[boundary_start + 1];
    const auto& boundary_b_first =
        additive.targets()[boundary_start + 2];
    const auto& boundary_b_second =
        additive.targets()[boundary_start + 3];
    const double boundary_a_center_x =
        0.5 * (boundary_a_first.x + boundary_a_second.x);
    const double boundary_a_center_y =
        0.5 * (boundary_a_first.y + boundary_a_second.y);
    const double boundary_b_center_x =
        0.5 * (boundary_b_first.x + boundary_b_second.x);
    const double boundary_b_center_y =
        0.5 * (boundary_b_first.y + boundary_b_second.y);
    ok &= require(
        boundary_pair_a.accepted && boundary_pair_b.accepted &&
            boundary_pair_a.added_target_ids.size() == 2 &&
            boundary_pair_b.added_target_ids.size() == 2 &&
            std::hypot(
                boundary_a_center_x - boundary_b_center_x,
                boundary_a_center_y - boundary_b_center_y) >
                1000.0,
        "repeated boundary crossing pairs must use different face seams");

    const int32_t removed_target = random.added_target_ids.front();
    const auto removed = additive.remove_target(removed_target);
    ok &= require(
        removed.accepted &&
            removed.removed_target_ids.size() == 1 &&
            additive.scenario_target_count(
                random.scenario_instance_id) == 3,
        "an individual target must be removable from its scenario");
    const auto removed_group =
        additive.remove_scenario(orbit_a.scenario_instance_id);
    ok &= require(
        removed_group.accepted &&
            removed_group.removed_target_ids.size() == 1,
        "an entire scenario instance must be removable");
    const int32_t prior_high_id =
        boundary_pair_b.added_target_ids.back();
    const auto cleared = additive.clear_all();
    ok &= require(
        cleared.accepted && additive.targets().empty() &&
            additive.scenarios().empty(),
        "clear-all must leave target_gen running with zero targets");
    const auto after_clear =
        additive.add_scenario(TargetScenario::kOrbitScenario);
    ok &= require(
        after_clear.accepted &&
            after_clear.added_target_ids.front() > prior_high_id,
        "target IDs must remain monotonic after clear-all");

    TargetScenario capacity(1);
    const auto almost_full =
        capacity.add_scenario(
            TargetScenario::kRandomFleetScenario, 254);
    const auto rejected_pair =
        capacity.add_scenario(
            TargetScenario::kCrossingPairScenario);
    ok &= require(
        almost_full.accepted && !rejected_pair.accepted &&
            capacity.targets().size() == 255,
        "a fixed two-target scenario must honor the 256-target limit");

    // One-shot minimum-range profile: continuous motion, 15 seconds inside
    // 2 km, a longer-than-coast blind interval, and disposal at 20 km outbound.
    TargetScenario flyby(1);
    flyby.clear_all();
    const auto flyby_added =
        flyby.add_scenario(TargetScenario::kMinimumRangeScenario);
    ok &= require(
        flyby_added.accepted && flyby.targets().size() == 1 &&
            std::abs(slant_range(flyby, flyby.targets().front()) -
                     8000.0) < 1.0e-6,
        "minimum-range transit must add one target inbound at 8 km");
    double time_inside_2km = 0.0;
    double time_inside_3km = 0.0;
    double last_visible_range =
        slant_range(flyby, flyby.targets().front());
    bool flyby_completed = false;
    for (int step = 0; step < 10'000 && !flyby_completed; ++step) {
        const auto update = flyby.step(kDt, step * kDt);
        if (!flyby.targets().empty()) {
            const double range = slant_range(
                flyby, flyby.targets().front());
            last_visible_range = range;
            if (range < 2000.0)
                time_inside_2km += kDt;
            if (range < 2998.0)
                time_inside_3km += kDt;
        }
        if (!update.completed_scenario_ids.empty()) {
            flyby_completed = true;
            ok &= require(
                update.completed_scenario_ids.front() ==
                    flyby_added.scenario_instance_id &&
                    update.removed_target_ids ==
                        flyby_added.added_target_ids,
                "flyby completion must report its scenario and disposed target");
        }
    }
    ok &= require(
        flyby_completed && flyby.targets().empty() &&
            flyby.scenarios().empty() &&
            last_visible_range >
                TargetScenario::kTransitEndRangeM -
                    2.0 * TargetScenario::kTransitSpeedMps * kDt &&
            last_visible_range <= TargetScenario::kTransitEndRangeM,
        "flyby must vanish automatically at 20 km outbound");
    ok &= require(
        std::abs(time_inside_2km -
                 TargetScenario::kTransitInnerDurationS) <= 2.0 * kDt,
        "flyby must spend 15 seconds inside 2 km");
    ok &= require(
        time_inside_3km > 23.0,
        "flyby must remain inside the 3 km blind range long enough "
        "to exceed the 12-second tracker coast");

    // One fighter traverses a short constant-range arc centered on the
    // Forward-Starboard/Aft-Starboard boundary at 90 degrees.
    TargetScenario handoff(1);
    handoff.clear_all();
    const auto handoff_added =
        handoff.add_scenario(TargetScenario::kFaceSeamScenario);
    ok &= require(
        handoff_added.accepted && handoff.targets().size() == 1 &&
            std::abs(slant_range(
                handoff, handoff.targets().front()) -
                TargetScenario::kFaceSeamRangeM) < 1.0e-6 &&
            std::abs(ship_relative_azimuth_deg(
                handoff, handoff.targets().front()) - 78.0) <
                1.0e-6,
        "face-seam handoff must start 12 degrees before the 90-degree seam");
    double handoff_last_azimuth =
        ship_relative_azimuth_deg(
            handoff, handoff.targets().front());
    double handoff_max_range_error = 0.0;
    bool handoff_completed = false;
    for (int step = 0;
         step < 5'000 && !handoff_completed; ++step) {
        const auto update = handoff.step(kDt, step * kDt);
        if (!handoff.targets().empty()) {
            handoff_last_azimuth =
                ship_relative_azimuth_deg(
                    handoff, handoff.targets().front());
            handoff_max_range_error = std::max(
                handoff_max_range_error,
                std::abs(
                    slant_range(
                        handoff, handoff.targets().front()) -
                    TargetScenario::kFaceSeamRangeM));
        }
        if (!update.completed_scenario_ids.empty()) {
            handoff_completed = true;
            ok &= require(
                update.completed_scenario_ids.size() == 1 &&
                    update.completed_scenario_ids.front() ==
                        handoff_added.scenario_instance_id &&
                    update.removed_target_ids ==
                        handoff_added.added_target_ids,
                "face-seam completion must report its disposed target");
        }
    }
    ok &= require(
        handoff_completed && handoff.targets().empty() &&
            handoff.scenarios().empty() &&
            handoff_last_azimuth > 101.9 &&
            handoff_last_azimuth < 102.0 &&
            handoff_max_range_error < 1.0e-6,
        "face-seam handoff must cross 90 degrees and finish at 102 degrees");

    // Two converging fighters share inward radial velocity while their
    // opposing tangential components create a clean crossing presentation.
    TargetScenario crossing(1);
    crossing.clear_all();
    const auto crossing_added =
        crossing.add_scenario(TargetScenario::kCrossingPairScenario);
    ok &= require(
        crossing_added.accepted &&
            crossing_added.added_target_ids.size() == 2 &&
            crossing.targets().size() == 2 &&
            crossing.targets()[0].motion ==
                TargetMotion::CrossingPair &&
            crossing.targets()[1].motion ==
                TargetMotion::CrossingPair,
        "crossing-pair scenario must add two one-shot fighters");
    double minimum_horizontal_separation =
        std::numeric_limits<double>::infinity();
    double altitude_separation = 0.0;
    bool crossing_completed = false;
    for (int step = 0;
         step < 5'000 && !crossing_completed; ++step) {
        const auto update = crossing.step(kDt, step * kDt);
        if (crossing.targets().size() == 2) {
            const auto& first = crossing.targets()[0];
            const auto& second = crossing.targets()[1];
            minimum_horizontal_separation = std::min(
                minimum_horizontal_separation,
                std::hypot(
                    first.x - second.x,
                    first.y - second.y));
            altitude_separation =
                std::abs(first.z - second.z);
        }
        if (!update.completed_scenario_ids.empty()) {
            crossing_completed = true;
            ok &= require(
                update.completed_scenario_ids.size() == 1 &&
                    update.completed_scenario_ids.front() ==
                        crossing_added.scenario_instance_id &&
                    update.removed_target_ids ==
                        crossing_added.added_target_ids,
                "crossing pair must complete once after disposing both targets");
        }
    }
    ok &= require(
        crossing_completed && crossing.targets().empty() &&
            crossing.scenarios().empty() &&
            std::abs(
                minimum_horizontal_separation -
                TargetScenario::kCrossingHorizontalMissM) <
                1.0e-6 &&
            std::abs(
                altitude_separation -
                TargetScenario::kCrossingAltitudeSeparationM) <
                1.0e-6,
        "crossing pair must preserve its 500 m horizontal and 300 m "
        "vertical separation");

    // This variant places the same crossing geometry directly on the
    // Forward-Port/Forward-Starboard seam at zero degrees ship-relative.
    TargetScenario boundary_crossing(1);
    boundary_crossing.clear_all();
    const auto boundary_crossing_added =
        boundary_crossing.add_scenario(
            TargetScenario::kFaceBoundaryCrossingPairScenario);
    ok &= require(
        boundary_crossing_added.accepted &&
            boundary_crossing.targets().size() == 2 &&
            ship_relative_azimuth_deg(
                boundary_crossing,
                boundary_crossing.targets()[0]) > 330.0 &&
            ship_relative_azimuth_deg(
                boundary_crossing,
                boundary_crossing.targets()[1]) < 30.0,
        "boundary pair must begin on opposite sides of the zero-degree seam");
    double boundary_minimum_separation =
        std::numeric_limits<double>::infinity();
    double first_azimuth_at_crossing = 180.0;
    double second_azimuth_at_crossing = 180.0;
    bool exchanged_faces = false;
    bool boundary_crossing_completed = false;
    for (int step = 0;
         step < 5'000 && !boundary_crossing_completed; ++step) {
        const auto update =
            boundary_crossing.step(kDt, step * kDt);
        if (boundary_crossing.targets().size() == 2) {
            const auto& first = boundary_crossing.targets()[0];
            const auto& second = boundary_crossing.targets()[1];
            const double separation = std::hypot(
                first.x - second.x,
                first.y - second.y);
            if (separation < boundary_minimum_separation) {
                boundary_minimum_separation = separation;
                first_azimuth_at_crossing =
                    ship_relative_azimuth_deg(
                        boundary_crossing, first);
                second_azimuth_at_crossing =
                    ship_relative_azimuth_deg(
                        boundary_crossing, second);
            }
            if (first.path_progress_m >
                0.5 * TargetScenario::kCrossingPathLengthM +
                    1000.0) {
                const double first_azimuth =
                    ship_relative_azimuth_deg(
                        boundary_crossing, first);
                const double second_azimuth =
                    ship_relative_azimuth_deg(
                        boundary_crossing, second);
                exchanged_faces =
                    exchanged_faces ||
                    (first_azimuth < 30.0 &&
                     second_azimuth > 330.0);
            }
        }
        if (!update.completed_scenario_ids.empty()) {
            boundary_crossing_completed = true;
            ok &= require(
                update.completed_scenario_ids.size() == 1 &&
                    update.completed_scenario_ids.front() ==
                        boundary_crossing_added.scenario_instance_id &&
                    update.removed_target_ids ==
                        boundary_crossing_added.added_target_ids,
                "boundary pair must complete once after both targets dispose");
        }
    }
    const double first_seam_error = std::min(
        first_azimuth_at_crossing,
        360.0 - first_azimuth_at_crossing);
    const double second_seam_error = std::min(
        second_azimuth_at_crossing,
        360.0 - second_azimuth_at_crossing);
    ok &= require(
        boundary_crossing_completed && exchanged_faces &&
            std::abs(
                boundary_minimum_separation -
                TargetScenario::kCrossingHorizontalMissM) <
                1.0e-6 &&
            first_seam_error < 1.0e-6 &&
            second_seam_error < 1.0e-6,
        "boundary pair must cross exactly on the seam and exchange faces");

    // Preserve the original deterministic long-duration random-fleet soak.
    std::map<int32_t, int> respawn_count;
    for (int step = 0; step < kSteps; ++step) {
        const double sim_time_s = step * kDt;
        const auto update = scenario.step(kDt, sim_time_s);
        const auto repeated = repeat.step(kDt, sim_time_s);
        ok &= require(
            update.respawned_target_ids ==
                    repeated.respawned_target_ids &&
                update.removed_target_ids ==
                    repeated.removed_target_ids,
            "fixed seed must reproduce the periodic respawn schedule");
        for (const int32_t id : update.respawned_target_ids)
            ++respawn_count[id];

        if (step % 500 == 0 ||
            !update.respawned_target_ids.empty()) {
            ok &= require(
                scenario.targets().size() == kRegressionTargets,
                "respawn must not change fleet size");
            for (std::size_t i = 0;
                 i < scenario.targets().size(); ++i) {
                const auto& target = scenario.targets()[i];
                const auto& duplicate = repeat.targets()[i];
                const double horizontal_range = std::hypot(
                    target.x - scenario.ship_east_m(),
                    target.y - scenario.ship_north_m());
                ok &= require(
                    same_state(target, duplicate),
                    "fixed seed must reproduce all target kinematics");
                ok &= require(
                    std::isfinite(target.x) &&
                        std::isfinite(target.y) &&
                        std::isfinite(target.z) &&
                        horizontal_range <= 120000.001,
                    "target state must stay finite and inside coverage");
                if (target.motion == TargetMotion::Orbit) {
                    const double relative_x =
                        target.x - scenario.ship_east_m();
                    const double relative_y =
                        target.y - scenario.ship_north_m();
                    const double relative_velocity_x =
                        target.velocity_x_mps -
                        TargetScenario::kShipSpeedMps *
                            std::sin(
                                TargetScenario::kShipHeadingDeg *
                                3.14159265358979323846 / 180.0);
                    const double relative_velocity_y =
                        target.velocity_y_mps -
                        TargetScenario::kShipSpeedMps *
                            std::cos(
                                TargetScenario::kShipHeadingDeg *
                                3.14159265358979323846 / 180.0);
                    const double radial_velocity =
                        relative_x * relative_velocity_x +
                        relative_y * relative_velocity_y;
                    ok &= require(
                        std::abs(slant_range(scenario, target) -
                                 TargetScenario::kBaselineRangeM) <
                            1.0e-6,
                        "orbit must remain at exactly 12 km");
                    ok &= require(
                        std::abs(radial_velocity) < 1.0e-5,
                        "orbit velocity must remain tangent");
                }
                if (target.type == ScenarioTargetType::Missile) {
                    ok &= require(
                        target.z >= 200.0,
                        "missile dive must retain its 200 m floor");
                }
            }
        }
    }

    int total_respawns = 0;
    int repeatedly_recycled = 0;
    for (const auto& [id, count] : respawn_count) {
        total_respawns += count;
        if (count >= 2)
            ++repeatedly_recycled;
        ok &= require(
            ids.count(id) == 1,
            "only configured random targets may respawn");
    }
    ok &= require(
        total_respawns >= 16,
        "30-minute live run must recycle a meaningful number of targets");
    ok &= require(
        repeatedly_recycled >= 4,
        "multiple targets must recycle periodically");
    ok &= require(
        respawn_count.count(TargetScenario::kBaselineTargetId) == 0,
        "orbit target must never be respawned");

    if (!ok)
        return 1;
    std::printf(
        "PASS: additive scenario lifecycle, minimum-range transit, and "
        "30-minute deterministic fleet (%d respawns)\n",
        total_respawns);
    return 0;
}
