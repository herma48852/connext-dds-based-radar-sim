// ============================================================================
// Radar simulation application - AESA SPY-6 class phased array radar.
//
// Internal components communicate exclusively over DDS topics (same domain):
//   Radar.BeamScheduler        -> Radar/BeamCommand          (50 Hz)
//   Radar.DetectionProcessor   -> Radar/RawReturn (1 kHz), Radar/DetectionEvent
//   Radar.TrackManager         -> Radar/TargetTrack          (10 Hz)
//   Radar.CalibrationMonitor   -> Radar/CalibrationStatus    (1 Hz)
//   Radar.CommandHandler       <- Radar/SystemCommand        (WaitSet)
//   Radar.ShipINS              -> Ship/ShipPosition          (10 Hz)
//   Radar.CommandConsole       -> Radar/SystemCommand        (UI scenarios)
//   Radar.HMI-UI               <- TargetTrack, DetectionEvent,
//                                 ShipPosition, CalibrationStatus (display)
//
// Usage: radar_app [--domain N]     (default domain 0)
// ============================================================================

#include <cstdlib>
#include <cstring>
#include <iostream>

// Crash diagnostics: dump a backtrace to stderr on fatal signals so the
// failing thread is identifiable without a debugger attached (POSIX only).
#if defined(__unix__) || defined(__APPLE__)
#include <cstdio>
#include <csignal>
#include <execinfo.h>
#include <unistd.h>
namespace {
void crash_handler(int sig) {
    void* frames[64];
    const int n = ::backtrace(frames, 64);
    char buf[128];
    const int len = std::snprintf(buf, sizeof(buf),
        "\n[radar_app] fatal signal %d (%s) — backtrace:\n", sig,
        sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : "SIGABRT");
    if (len > 0) (void)!::write(STDERR_FILENO, buf, static_cast<size_t>(len));
    ::backtrace_symbols_fd(frames, n, STDERR_FILENO);
    ::_exit(128 + sig);
}
void install_crash_handler() {
    std::signal(SIGSEGV, crash_handler);
    std::signal(SIGBUS,  crash_handler);
    std::signal(SIGABRT, crash_handler);
}
} // namespace
#else
void install_crash_handler() {}
#endif

#include "CommandConsole.hpp"
#include "DataBus.hpp"
#include "ShipSimulator.hpp"
#include "SimClock.hpp"
#include "components/BeamScheduler.hpp"
#include "components/CalibrationMonitor.hpp"
#include "components/CommandHandler.hpp"
#include "components/DetectionProcessor.hpp"
#include "components/HmiUi.hpp"
#include "components/TrackManager.hpp"
#include "ui/UiApp.hpp"

int main(int argc, char** argv) {
    install_crash_handler();
    int32_t domain = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--domain") == 0 && i + 1 < argc)
            domain = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--help") == 0) {
            std::cout << "radar_app [--domain N]\n";
            return 0;
        }
    }

    radar::SimClock::start();
    std::cout << "[radar_app] starting on DDS domain " << domain << "\n";

    radar::app::DataBus bus;

    radar::app::ShipSimulator      ship(domain, bus);
    radar::app::BeamScheduler      scheduler(domain, bus);
    radar::app::DetectionProcessor processor(domain, bus);
    radar::app::TrackManager       tracker(domain, bus);
    radar::app::CalibrationMonitor calibration(domain, bus);
    radar::app::CommandHandler     commands(domain, bus);
    radar::app::CommandConsole     console(domain);
    radar::app::HmiUi              hmi(domain, bus);

    // Order matters only for startup cosmetics; DDS discovery handles wiring.
    ship.start();
    scheduler.start();
    processor.start();
    tracker.start();
    calibration.start();
    commands.start();
    console.start();
    hmi.start();

    radar::ui::UiApp app(bus, console);
    const int rc = app.run(); // blocks until the window closes

    std::cout << "[radar_app] shutting down\n";
    hmi.stop();
    console.stop();
    commands.stop();
    calibration.stop();
    tracker.stop();
    processor.stop();
    scheduler.stop();
    ship.stop();
    return rc;
}
