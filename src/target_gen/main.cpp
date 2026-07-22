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
// Usage: target_gen [--domain N] [--targets N] [scenarios...]
// ============================================================================

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "DiagnosticsInjector.hpp"
#include "SimClock.hpp"
#include "TargetFleet.hpp"

namespace {
std::atomic<bool> g_running{true};
void on_sigint(int) { g_running.store(false); }
}

int main(int argc, char** argv) {
    radds::disable_monitoring_lib(); // no monitoring DPs (see DdsSupport)
    int32_t domain = 0;
    int num_targets = 8;
    double respawn_km = 120.0;
    bool qos_mismatch = false, type_mismatch = false, degrade = false;
    std::string rma_offline;
    double run_seconds = 0.0;
    std::string stop_file;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--domain") == 0 && i + 1 < argc)
            domain = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--targets") == 0 && i + 1 < argc)
            num_targets = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--inject-qos-mismatch") == 0)
            qos_mismatch = true;
        else if (std::strcmp(argv[i], "--inject-type-mismatch") == 0)
            type_mismatch = true;
        else if (std::strcmp(argv[i], "--degrade-array") == 0)
            degrade = true;
        else if (std::strcmp(argv[i], "--respawn-range") == 0 && i + 1 < argc)
            respawn_km = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--rma-offline") == 0 && i + 1 < argc)
            rma_offline = argv[++i];
        else if (std::strcmp(argv[i], "--run-seconds") == 0 && i + 1 < argc)
            run_seconds = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--stop-file") == 0 && i + 1 < argc)
            stop_file = argv[++i];
        else if (std::strcmp(argv[i], "--help") == 0) {
            std::cout <<
                "target_gen [--domain N] [--targets N] [--respawn-range KM]\n"
                "           [--inject-qos-mismatch] [--inject-type-mismatch]\n"
                "           [--degrade-array] [--rma-offline N|all]\n"
                "           [--run-seconds N] [--stop-file PATH]\n"
                "  --respawn-range  targets past this ship-relative range are\n"
                "                   recycled inbound (default 120 km, 0 disables);\n"
                "                   keeps the demo picture busy indefinitely\n"
                "  --rma-offline    send CMD_RMA_OFFLINE at t+5s (RMA index\n"
                "                   0..15 or \"all\"); scripted degraded-array demo\n"
                "  --run-seconds    stop cleanly after N seconds (automation)\n"
                "  --stop-file      stop cleanly when PATH appears\n";
            return 0;
        }
    }

    if (run_seconds < 0.0) {
        std::cerr << "--run-seconds must be non-negative\n";
        return 2;
    }

    const auto process_started = std::chrono::steady_clock::now();
    const auto stop_requested = [&] {
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
              << " with " << num_targets << " targets\n";

    target_gen::TargetFleet fleet(domain, num_targets);
    fleet.set_respawn_range_km(respawn_km);
    fleet.start();

    target_gen::DiagnosticsInjector injector(domain);
    std::vector<std::thread> delayed_commands;
    if (qos_mismatch)  injector.inject_qos_mismatch();
    if (type_mismatch) injector.inject_type_mismatch();
    if (degrade) {
        delayed_commands.emplace_back([&injector] {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            injector.send_degrade_command();
        });
    }
    if (!rma_offline.empty()) {
        delayed_commands.emplace_back([&injector, p = rma_offline] {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            injector.send_rma_offline(p);
        });
    }

    std::cout << "[target_gen] running; Ctrl+C to stop\n";
    while (g_running.load() && !stop_requested())
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::cout << "[target_gen] shutting down\n";
    for (auto& thread : delayed_commands)
        thread.join();
    injector.stop();
    fleet.stop();
    return 0;
}
