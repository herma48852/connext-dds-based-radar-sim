// ============================================================================
// Target generator application - same DDS domain as the radar app.
//
// Publishes TargetGen/TargetTruth (per-target, keyed) and the ship-motion
// ground truth on Ship/ShipPosition (source_id = 1).
//
// Diagnostic scenarios for Connext Studio (all optional, combinable):
//   --inject-qos-mismatch    RELIABLE reader vs BEST_EFFORT DetectionEvent
//   --inject-type-mismatch   wrong type on the TargetGen/TargetTruth name
//   --degrade-array          sends SystemCommand(CMD_DEGRADE_ARRAY) at +5 s
//
// Usage: target_gen [--domain N] [--control-domain N] [--targets N] [...]
// ============================================================================

#include <atomic>
#include <chrono>
#include <csignal>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "DiagnosticsInjector.hpp"
#include "CliParse.hpp"
#include "RadarFaces.hpp"
#include "SimClock.hpp"
#include "TargetFleet.hpp"
#include "WorkerGuard.hpp"

namespace {
std::atomic<bool> g_running{true};
void on_sigint(int) { g_running.store(false); }
}

int run_target_gen(int argc, char** argv) {
    radds::disable_monitoring_lib(); // no monitoring DPs (see DdsSupport)
    int32_t domain = 0;
    int32_t control_domain = -1;
    int num_targets = 32;
    double respawn_km = 120.0;
    bool qos_mismatch = false, type_mismatch = false, degrade = false;
    radar::faces::FaceMask diagnostic_face_mask =
        radar::faces::kForwardStarboardMask;
    std::string rma_offline;
    double run_seconds = 0.0;
    std::string stop_file;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--domain") == 0) {
            if (++i >= argc ||
                !radar::cli::parse_integer<int32_t>(
                    argv[i], 0, 232, domain)) {
                std::cerr << "--domain must be an integer from 0 to 232\n";
                return 2;
            }
        }
        else if (std::strcmp(argv[i], "--control-domain") == 0) {
            if (++i >= argc ||
                !radar::cli::parse_integer<int32_t>(
                    argv[i], 0, 232, control_domain)) {
                std::cerr
                    << "--control-domain must be an integer from 0 to 232\n";
                return 2;
            }
        }
        else if (std::strcmp(argv[i], "--targets") == 0) {
            if (++i >= argc ||
                !radar::cli::parse_integer<int>(
                    argv[i], 1, 256, num_targets)) {
                std::cerr << "--targets must be an integer from 1 to 256\n";
                return 2;
            }
        }
        else if (std::strcmp(argv[i], "--inject-qos-mismatch") == 0)
            qos_mismatch = true;
        else if (std::strcmp(argv[i], "--inject-type-mismatch") == 0)
            type_mismatch = true;
        else if (std::strcmp(argv[i], "--degrade-array") == 0)
            degrade = true;
        else if (std::strcmp(argv[i], "--face") == 0) {
            if (++i >= argc) {
                std::cerr << "--face requires fs, as, ap, fp, or all\n";
                return 2;
            }
            const std::string face = argv[i];
            if (face == "fs")
                diagnostic_face_mask =
                    radar::faces::kForwardStarboardMask;
            else if (face == "as")
                diagnostic_face_mask =
                    radar::faces::kAftStarboardMask;
            else if (face == "ap")
                diagnostic_face_mask = radar::faces::kAftPortMask;
            else if (face == "fp")
                diagnostic_face_mask = radar::faces::kForwardPortMask;
            else if (face == "all")
                diagnostic_face_mask = radar::faces::kAllFacesMask;
            else {
                std::cerr << "--face requires fs, as, ap, fp, or all\n";
                return 2;
            }
        }
        else if (std::strcmp(argv[i], "--respawn-range") == 0) {
            if (++i >= argc ||
                !radar::cli::parse_finite_double(
                    argv[i], 0.0, 1000000.0, respawn_km)) {
                std::cerr << "--respawn-range must be between 0 and 1000000 km\n";
                return 2;
            }
        }
        else if (std::strcmp(argv[i], "--rma-offline") == 0) {
            if (++i >= argc) {
                std::cerr << "--rma-offline requires 0..15 or all\n";
                return 2;
            }
            if (std::strcmp(argv[i], "all") == 0) {
                rma_offline = "all";
            } else {
                int rma = 0;
                if (!radar::cli::parse_integer<int>(argv[i], 0, 15, rma)) {
                    std::cerr << "--rma-offline requires 0..15 or all\n";
                    return 2;
                }
                rma_offline = std::to_string(rma);
            }
        }
        else if (std::strcmp(argv[i], "--run-seconds") == 0) {
            if (++i >= argc ||
                !radar::cli::parse_finite_double(
                    argv[i], 0.0, 604800.0, run_seconds)) {
                std::cerr << "--run-seconds must be between 0 and 604800\n";
                return 2;
            }
        }
        else if (std::strcmp(argv[i], "--stop-file") == 0) {
            if (++i >= argc) {
                std::cerr << "--stop-file requires a path\n";
                return 2;
            }
            stop_file = argv[i];
        }
        else if (std::strcmp(argv[i], "--help") == 0) {
            std::cout <<
                "target_gen [--domain N] [--control-domain N]\n"
                "           [--targets N] [--respawn-range KM]\n"
                "           [--inject-qos-mismatch] [--inject-type-mismatch]\n"
                "           [--degrade-array] [--rma-offline N|all]\n"
                "           [--face fs|as|ap|fp|all]\n"
                "           [--run-seconds N] [--stop-file PATH]\n"
                "  --targets        total targets: one deterministic 12 km\n"
                "                   baseline orbit plus N-1 randomized inbound\n"
                "  --control-domain separate DDS domain used only by the\n"
                "                   target_control UI (default: domain + 1)\n"
                "  --respawn-range  randomized targets past this ship-relative range are\n"
                "                   recycled inbound (default 120 km, 0 disables);\n"
                "                   keeps the demo picture busy indefinitely\n"
                "  --rma-offline    send CMD_RMA_OFFLINE at t+5s (RMA index\n"
                "                   0..15 or \"all\"); scripted degraded-array demo\n"
                "  --face           target face for degrade/RMA scenarios\n"
                "                   (default fs; use all for all four faces)\n"
                "  --run-seconds    stop cleanly after N seconds (automation)\n"
                "  --stop-file      stop cleanly when PATH appears\n";
            return 0;
        }
    }

    if (control_domain < 0)
        control_domain = (domain + 1) % 233;
    if (control_domain == domain) {
        std::cerr
            << "--control-domain must differ from the simulation --domain\n";
        return 2;
    }

    const auto process_started = std::chrono::steady_clock::now();
    const auto stop_requested = [&] {
        if (radar::worker_failure_requested())
            return true;
        if (run_seconds > 0.0 &&
            std::chrono::duration<double>(std::chrono::steady_clock::now()
                                          - process_started).count() >= run_seconds)
            return true;
        if (!stop_file.empty()) {
            std::error_code ec;
            if (std::filesystem::exists(stop_file, ec) && !ec)
                return true;
        }
        return false;
    };

    radar::SimClock::start();
    std::signal(SIGINT, on_sigint);

    std::cout << "[target_gen] starting on DDS domain " << domain
              << " with " << num_targets << " targets (1 baseline + "
              << (num_targets - 1) << " randomized); control domain "
              << control_domain << "\n";

    target_gen::TargetFleet fleet(domain, control_domain, num_targets);
    fleet.set_respawn_range_km(respawn_km);
    fleet.start();

    target_gen::DiagnosticsInjector injector(domain);
    std::vector<std::thread> delayed_commands;
    std::mutex delayed_mutex;
    std::condition_variable delayed_cv;
    bool cancel_delayed_commands = false;
    const auto delay_completed = [&] {
        std::unique_lock lock(delayed_mutex);
        return !delayed_cv.wait_for(
            lock, std::chrono::seconds(5),
            [&] { return cancel_delayed_commands; });
    };
    if (qos_mismatch)  injector.inject_qos_mismatch();
    if (type_mismatch) injector.inject_type_mismatch();
    if (degrade) {
        delayed_commands.emplace_back(
            [&injector, &delay_completed, diagnostic_face_mask] {
                radar::run_worker_guarded(
                    "TargetGen.DelayedDegrade", [&] {
                        if (delay_completed()) {
                            injector.send_degrade_command(
                                diagnostic_face_mask);
                        }
                    });
            });
    }
    if (!rma_offline.empty()) {
        delayed_commands.emplace_back(
            [&injector, &delay_completed, p = rma_offline,
             diagnostic_face_mask] {
                radar::run_worker_guarded("TargetGen.DelayedRma", [&] {
                    if (delay_completed())
                        injector.send_rma_offline(
                            p, diagnostic_face_mask);
                });
            });
    }

    std::cout << "[target_gen] running; Ctrl+C to stop\n";
    while (g_running.load() && !stop_requested())
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

    {
        std::lock_guard lock(delayed_mutex);
        cancel_delayed_commands = true;
    }
    delayed_cv.notify_all();
    for (auto& thread : delayed_commands)
        thread.join();
    std::cout << "[target_gen] shutting down\n";
    injector.stop();
    fleet.stop();
    return 0;
}

int main(int argc, char** argv) {
    try {
        return run_target_gen(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "[target_gen] startup/runtime exception: "
                  << e.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "[target_gen] startup/runtime exception: unknown exception\n";
        return 1;
    }
}
