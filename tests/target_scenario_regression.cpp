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
    return a.id == b.id && a.type == b.type && a.x == b.x && a.y == b.y &&
           a.z == b.z && a.speed_mps == b.speed_mps &&
           a.heading_deg == b.heading_deg && a.rcs_dbsm == b.rcs_dbsm &&
           a.maneuver == b.maneuver && a.phase == b.phase &&
           a.profile == b.profile &&
           a.velocity_x_mps == b.velocity_x_mps &&
           a.velocity_y_mps == b.velocity_y_mps;
}

} // namespace

int main() {
    constexpr int kWebinarTargets = 16;
    constexpr double kDt = 0.02;
    constexpr int kSteps = 90'000; // 30 live minutes, accelerated (no sleeps)

    TargetScenario scenario(kWebinarTargets);
    TargetScenario repeat(kWebinarTargets);
    bool ok = true;

    ok &= require(scenario.targets().size() == kWebinarTargets,
                  "webinar scenario must contain exactly 16 targets");
    ok &= require(scenario.respawn_range_km() == 120.0,
                  "default periodic respawn range must remain 120 km");

    const std::array<int, 6> expected_types{4, 2, 4, 2, 2, 2};
    std::array<int, 6> observed_types{};
    std::set<int32_t> ids;
    for (size_t i = 0; i < scenario.targets().size(); ++i) {
        const auto& target = scenario.targets()[i];
        ids.insert(target.id);
        ++observed_types[static_cast<size_t>(target.type)];
        ok &= require(target.id == 100 + static_cast<int32_t>(i),
                      "target IDs must remain the stable 100..115 sequence");
        ok &= require(target.profile == static_cast<int>(i % 8),
                      "the eight-profile live mix must repeat twice");
        ok &= require(same_state(target, repeat.targets()[i]),
                      "fixed seed must reproduce the initial webinar picture");
    }
    ok &= require(ids.size() == kWebinarTargets, "target IDs must be unique");
    ok &= require(observed_types == expected_types,
                  "16-target mix must remain 4 fighters, 2 bombers, 4 missiles, "
                  "2 ships, 2 drones, and 2 decoys");

    std::map<int32_t, int> respawn_count;
    for (int step = 0; step < kSteps; ++step) {
        const double sim_time_s = step * kDt;
        const auto respawned = scenario.step(kDt, sim_time_s);
        const auto repeated = repeat.step(kDt, sim_time_s);
        ok &= require(respawned == repeated,
                      "fixed seed must reproduce the periodic respawn schedule");
        for (const int32_t id : respawned) ++respawn_count[id];

        if (step % 500 == 0 || !respawned.empty()) {
            ok &= require(scenario.targets().size() == kWebinarTargets,
                          "respawn must recycle targets, never add or remove them");
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
        ok &= require(ids.count(id) == 1, "only webinar target IDs may respawn");
    }
    ok &= require(total_respawns >= 16,
                  "30-minute live run must recycle a meaningful number of targets");
    ok &= require(repeatedly_recycled >= 4,
                  "multiple targets must recycle periodically, not only once");

    if (!ok) return 1;
    std::printf("PASS: 16-target webinar scenario stayed bounded with %d respawns "
                "across %zu targets\n",
                total_respawns, respawn_count.size());
    return 0;
}
