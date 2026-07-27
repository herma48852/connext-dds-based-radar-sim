#include "TargetScenario.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>

namespace {

using target_gen::ScenarioTargetType;
using target_gen::TargetScenario;
using target_gen::TargetState;

bool require(bool condition, const std::string& message) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message.c_str());
    return condition;
}

bool same_state(const TargetState& a, const TargetState& b) {
    return a.id == b.id && a.type == b.type &&
           a.baseline_orbit == b.baseline_orbit &&
           a.x == b.x && a.y == b.y && a.z == b.z &&
           a.speed_mps == b.speed_mps &&
           a.heading_deg == b.heading_deg && a.rcs_dbsm == b.rcs_dbsm &&
           a.maneuver == b.maneuver && a.phase == b.phase &&
           a.profile == b.profile &&
           a.velocity_x_mps == b.velocity_x_mps &&
           a.velocity_y_mps == b.velocity_y_mps;
}

} // namespace

int main() {
    constexpr int kRegressionTargets = 16;
    constexpr double kDt = 0.02;
    constexpr int kSteps = 90'000; // 30 live minutes, accelerated (no sleeps)

    TargetScenario scenario(kRegressionTargets);
    TargetScenario repeat(kRegressionTargets);
    TargetScenario baseline_only(1);
    bool ok = true;

    ok &= require(baseline_only.targets().size() == 1 &&
                      baseline_only.targets().front().baseline_orbit,
                  "a one-target run must contain only the baseline orbit");
    ok &= require(scenario.targets().size() == kRegressionTargets,
                  "scenario must contain the baseline plus 15 randomized targets");
    ok &= require(scenario.respawn_range_km() == 120.0,
                  "default periodic respawn range must remain 120 km");

    const auto& baseline = scenario.targets().front();
    const double baseline_horizontal_range =
        std::hypot(baseline.x, baseline.y);
    const double baseline_slant_range =
        std::hypot(baseline_horizontal_range, baseline.z);
    ok &= require(baseline.id == TargetScenario::kBaselineTargetId &&
                      baseline.baseline_orbit &&
                      baseline.type == ScenarioTargetType::Fighter,
                  "baseline orbit must use its stable fighter instance");
    ok &= require(
        std::abs(baseline_slant_range - TargetScenario::kBaselineRangeM) <
                1.0e-6 &&
            std::abs(
                std::atan2(baseline.z, baseline_horizontal_range) *
                        180.0 / 3.14159265358979323846 -
                    TargetScenario::kBaselineElevationDeg) <
                1.0e-12,
        "baseline must start at 12 km on the 14-degree elevation bar");

    const std::array<int, 6> expected_types{5, 2, 4, 2, 2, 1};
    std::array<int, 6> observed_types{};
    std::set<int32_t> ids;
    for (size_t i = 0; i < scenario.targets().size(); ++i) {
        const auto& target = scenario.targets()[i];
        ids.insert(target.id);
        ++observed_types[static_cast<size_t>(target.type)];
        if (i == 0) {
            ok &= require(target.profile == -1,
                          "baseline must remain outside the randomized profiles");
        } else {
            const auto randomized_index = static_cast<int32_t>(i - 1);
            ok &= require(target.id == 100 + randomized_index,
                          "randomized target IDs must remain stable at 100..114");
            ok &= require(target.profile ==
                              static_cast<int>(randomized_index % 8),
                          "the eight-profile live sequence must remain stable");
        }
        ok &= require(same_state(target, repeat.targets()[i]),
                      "fixed seed must reproduce the initial regression picture");
    }
    ok &= require(ids.size() == kRegressionTargets,
                  "baseline and randomized target IDs must be unique");
    ok &= require(observed_types == expected_types,
                  "fleet must contain the baseline fighter plus the unchanged "
                  "15-target randomized profile sequence");

    std::map<int32_t, int> respawn_count;
    for (int step = 0; step < kSteps; ++step) {
        const double sim_time_s = step * kDt;
        const auto respawned = scenario.step(kDt, sim_time_s);
        const auto repeated = repeat.step(kDt, sim_time_s);
        ok &= require(respawned == repeated,
                      "fixed seed must reproduce the periodic respawn schedule");
        for (const int32_t id : respawned) ++respawn_count[id];

        if (step % 500 == 0 || !respawned.empty()) {
            ok &= require(scenario.targets().size() == kRegressionTargets,
                          "respawn must recycle randomized targets without "
                          "changing fleet size");
            for (size_t i = 0; i < scenario.targets().size(); ++i) {
                const auto& target = scenario.targets()[i];
                const auto& duplicate = repeat.targets()[i];
                const double range = std::hypot(
                    target.x - scenario.ship_east_m(),
                    target.y - scenario.ship_north_m());
                ok &= require(same_state(target, duplicate),
                              "fixed seed must reproduce all target kinematics");
                ok &= require(std::isfinite(target.x) && std::isfinite(target.y) &&
                                  std::isfinite(target.z) && range <= 120000.001,
                              "target state must stay finite and inside coverage");
                if (target.baseline_orbit) {
                    const double relative_x =
                        target.x - scenario.ship_east_m();
                    const double relative_y =
                        target.y - scenario.ship_north_m();
                    const double slant_range =
                        std::hypot(std::hypot(relative_x, relative_y), target.z);
                    const double relative_velocity_x =
                        target.velocity_x_mps -
                        TargetScenario::kShipSpeedMps *
                            std::sin(TargetScenario::kShipHeadingDeg *
                                     3.14159265358979323846 / 180.0);
                    const double relative_velocity_y =
                        target.velocity_y_mps -
                        TargetScenario::kShipSpeedMps *
                            std::cos(TargetScenario::kShipHeadingDeg *
                                     3.14159265358979323846 / 180.0);
                    const double radial_velocity =
                        relative_x * relative_velocity_x +
                        relative_y * relative_velocity_y;
                    ok &= require(
                        std::abs(slant_range -
                                 TargetScenario::kBaselineRangeM) < 1.0e-6,
                        "baseline orbit must remain at exactly 12 km");
                    ok &= require(std::abs(radial_velocity) < 1.0e-5,
                                  "baseline velocity must remain tangent "
                                  "to the ship-relative orbit");
                }
                if (target.type == ScenarioTargetType::Missile)
                    ok &= require(target.z >= 200.0,
                                  "missile dive must retain its 200 m floor");
            }
        }
    }

    int total_respawns = 0;
    int repeatedly_recycled = 0;
    for (const auto& [id, count] : respawn_count) {
        total_respawns += count;
        if (count >= 2) ++repeatedly_recycled;
        ok &= require(ids.count(id) == 1, "only configured target IDs may respawn");
    }
    ok &= require(total_respawns >= 16,
                  "30-minute live run must recycle a meaningful number of targets");
    ok &= require(repeatedly_recycled >= 4,
                  "multiple targets must recycle periodically, not only once");
    ok &= require(respawn_count.count(TargetScenario::kBaselineTargetId) == 0,
                  "baseline orbit must never be respawned");

    if (!ok) return 1;
    std::printf("PASS: 16-target baseline scenario stayed "
                "bounded with %d respawns across %zu randomized targets\n",
                total_respawns, respawn_count.size());
    return 0;
}
