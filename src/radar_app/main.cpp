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

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>

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

namespace {
std::atomic<bool> g_running{true};
void on_sigint(int) { g_running.store(false); }
} // namespace

int main(int argc, char** argv) {
    install_crash_handler();
    int32_t domain = 0;
    bool headless = false;
    bool no_dispose = false;
    bool gl_throttle = false;
    int swap_interval = 1;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--domain") == 0 && i + 1 < argc)
            domain = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--headless") == 0)
            headless = true;
        else if (std::strcmp(argv[i], "--no-dispose") == 0)
            no_dispose = true;
        else if (std::strcmp(argv[i], "--gl-throttle") == 0)
            gl_throttle = true;
        else if (std::strcmp(argv[i], "--swap-interval") == 0 && i + 1 < argc)
            swap_interval = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--help") == 0) {
            std::cout << "radar_app [--domain N] [--headless] [--no-dispose]\n"
                         "          [--gl-throttle] [--swap-interval N]\n"
                         "  --headless       components only (no window); crash-bisect\n"
                         "                   soak: same DDS traffic, zero GLFW/ImGui/AppKit\n"
                         "  --no-dispose     TrackManager never calls dispose_instance\n"
                         "  --gl-throttle    upload the B-scope texture every 4th frame\n"
                         "                   (15 Hz); tests GL driver load as crash suspect\n"
                         "  --swap-interval  glfwSwapInterval(N); 2 halves all GL traffic\n"
                         "                   to 30 fps (default 1 = vsync 60 fps)\n";
            return 0;
        }
    }

    radar::SimClock::start();
    std::cout << "[radar_app] starting on DDS domain " << domain << "\n";

    radar::app::DataBus bus;
    if (no_dispose) {
        bus.dispose_enabled.store(false);
        std::cout << "[radar_app] --no-dispose: dispose_instance disabled\n";
    }

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

    int rc = 0;
    if (headless) {
        // Soak test for the SIGSEGV investigation: identical components,
        // participants and DDS traffic as the full app, but no GLFW window,
        // no ImGui and no AppKit. If this survives for hours while the
        // windowed build dies after ~1 minute, the corruptor lives in the
        // UI/windowing layer, not in the DDS/component layer.
        std::signal(SIGINT, on_sigint);
        std::cout << "[radar_app] headless soak mode; Ctrl+C to stop\n";
        while (g_running.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            std::cout << "[radar_app] alive t="
                      << radar::SimClock::sim_millis() / 1000 << " s\n";
        }
    } else {
        radar::ui::UiApp app(bus, console);
        if (gl_throttle) {
            app.set_bscope_upload_decimation(4);
            std::cout << "[radar_app] --gl-throttle: B-scope upload at 15 Hz\n";
        }
        if (swap_interval != 1) {
            app.set_swap_interval(swap_interval);
            std::cout << "[radar_app] swap interval " << swap_interval << "\n";
        }
        rc = app.run(); // blocks until the window closes
    }

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
