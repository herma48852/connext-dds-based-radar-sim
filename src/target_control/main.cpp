#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "CliParse.hpp"
#include "DdsSupport.hpp"
#include "SimClock.hpp"
#include "TargetControlClient.hpp"
#include "TargetControlUi.hpp"
#include "WorkerGuard.hpp"

namespace {

bool wait_for_reply(
        target_control::TargetControlClient& client,
        int64_t request_id) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto& reply : client.take_replies()) {
            if (reply.request_id != request_id)
                continue;
            if (reply.result !=
                radar::types::TargetControlResult::
                    TARGET_CONTROL_ACCEPTED) {
                std::cerr << "[target_control] request " << request_id
                          << " rejected: " << reply.message << "\n";
                return false;
            }
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::cerr << "[target_control] timed out waiting for request "
              << request_id << "\n";
    return false;
}

template <typename Predicate>
bool wait_for_snapshot(
        target_control::TargetControlClient& client,
        Predicate&& predicate) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto snapshot = client.snapshot();
        if (snapshot && predicate(*snapshot))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

int run_smoke_test(target_control::TargetControlClient& client) {
    if (!wait_for_snapshot(
            client, [](const auto& snapshot) {
                return snapshot.catalog.size() == 7;
            })) {
        std::cerr
            << "[target_control] no target_gen catalog on control domain "
            << client.domain_id() << "\n";
        return 1;
    }

    const int64_t clear_request = client.clear_all();
    if (!wait_for_reply(client, clear_request) ||
        !wait_for_snapshot(
            client, [](const auto& snapshot) {
                return snapshot.targets.empty() &&
                       snapshot.scenarios.empty();
            })) {
        return 1;
    }

    for (const auto& [name, count] :
         {std::pair{"orbit_12km", 1},
          std::pair{"orbit_12km", 1},
          std::pair{"random_fleet", 3},
          std::pair{"presentation_fleet", 6},
          std::pair{"minimum_range_transit", 1},
          std::pair{"minimum_range_transit", 1},
          std::pair{"face_seam_handoff", 1},
          std::pair{"crossing_pair", 2},
          std::pair{"face_boundary_crossing_pair", 2}}) {
        if (!wait_for_reply(
                client, client.add_scenario(name, count))) {
            return 1;
        }
    }

    if (!wait_for_snapshot(
            client, [](const auto& snapshot) {
                return snapshot.scenarios.size() == 9 &&
                       snapshot.targets.size() == 18;
            })) {
        std::cerr
            << "[target_control] additive inventory did not reach "
               "9 scenarios / 18 targets\n";
        return 1;
    }

    const auto inventory = client.snapshot();
    if (!inventory)
        return 1;
    const auto template_for = [&](int64_t scenario_instance_id) {
        const auto scenario = std::find_if(
            inventory->scenarios.begin(),
            inventory->scenarios.end(),
            [&](const auto& item) {
                return item.scenario_instance_id ==
                    scenario_instance_id;
            });
        return scenario == inventory->scenarios.end()
            ? std::string_view{}
            : std::string_view{scenario->template_name};
    };
    bool distinct_orbits = false;
    bool distinct_transits = false;
    for (std::size_t i = 0; i < inventory->targets.size(); ++i) {
        for (std::size_t j = i + 1; j < inventory->targets.size(); ++j) {
            const auto& a = inventory->targets[i];
            const auto& b = inventory->targets[j];
            const double separation = std::hypot(
                a.position.x_east_m - b.position.x_east_m,
                a.position.y_north_m - b.position.y_north_m);
            const std::string_view a_template =
                template_for(a.scenario_instance_id);
            const std::string_view b_template =
                template_for(b.scenario_instance_id);
            if (a_template == "orbit_12km" &&
                b_template == "orbit_12km" &&
                separation > 1000.0) {
                distinct_orbits = true;
            }
            if (a_template == "minimum_range_transit" &&
                b_template == "minimum_range_transit" &&
                separation > 1000.0) {
                distinct_transits = true;
            }
        }
    }
    if (!distinct_orbits || !distinct_transits) {
        std::cerr
            << "[target_control] repeated scenarios overlapped\n";
        return 1;
    }

    const int64_t final_clear = client.clear_all();
    if (!wait_for_reply(client, final_clear) ||
        !wait_for_snapshot(
            client, [](const auto& snapshot) {
                return snapshot.targets.empty();
            })) {
        return 1;
    }

    std::cout
        << "target_control smoke: PASS "
           "(seven-template catalog, additive launch, separation, "
           "clear-all)\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        int32_t control_domain = 93;
        bool smoke_test = false;
        double run_seconds = 0.0;
        std::string stop_file;
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--domain") == 0) {
                if (++i >= argc ||
                    !radar::cli::parse_integer<int32_t>(
                        argv[i], 0, 232, control_domain)) {
                    std::cerr
                        << "--domain must be an integer from 0 to 232\n";
                    return 2;
                }
            } else if (std::strcmp(argv[i], "--smoke-test") == 0) {
                smoke_test = true;
            } else if (std::strcmp(argv[i], "--run-seconds") == 0) {
                if (++i >= argc ||
                    !radar::cli::parse_finite_double(
                        argv[i], 0.0, 604800.0, run_seconds)) {
                    std::cerr
                        << "--run-seconds must be between 0 and 604800\n";
                    return 2;
                }
            } else if (std::strcmp(argv[i], "--stop-file") == 0) {
                if (++i >= argc) {
                    std::cerr << "--stop-file requires a path\n";
                    return 2;
                }
                stop_file = argv[i];
            } else if (std::strcmp(argv[i], "--help") == 0) {
                std::cout
                    << "target_control [--domain N] [--run-seconds N] "
                       "[--stop-file PATH] [--smoke-test]\n"
                    << "  --domain  target_gen's separate control DDS "
                       "domain (default 93)\n"
                    << "  --run-seconds  close the UI after N seconds\n"
                    << "  --stop-file  close the UI when PATH appears\n"
                    << "  --smoke-test  headless end-to-end control check\n";
                return 0;
            } else {
                std::cerr << "unknown option: " << argv[i] << "\n";
                return 2;
            }
        }

        radds::disable_monitoring_lib();
        radar::SimClock::start();
        target_control::TargetControlClient client(control_domain);
        client.start();
        if (smoke_test) {
            const int result = run_smoke_test(client);
            client.stop();
            return result;
        }
        target_control::TargetControlUi ui(client);
        const auto started = std::chrono::steady_clock::now();
        ui.set_stop_requested([=] {
            if (radar::worker_failure_requested())
                return true;
            if (run_seconds > 0.0 &&
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - started).count() >=
                    run_seconds) {
                return true;
            }
            if (!stop_file.empty()) {
                std::error_code ec;
                if (std::filesystem::exists(stop_file, ec) && !ec)
                    return true;
            }
            return false;
        });
        const int result = ui.run();
        client.stop();
        return result != 0 ? result
                           : (radar::worker_failure_requested() ? 1 : 0);
    } catch (const std::exception& error) {
        std::cerr << "[target_control] exception: "
                  << error.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "[target_control] unknown exception\n";
        return 1;
    }
}
