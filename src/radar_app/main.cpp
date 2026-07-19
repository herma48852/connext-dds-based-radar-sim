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
//
// Usage: radar_app [--domain N]     (default domain 0)
// ============================================================================

#include <cstdlib>
#include <cstring>
#include <iostream>

#include "CommandConsole.hpp"
#include "DataBus.hpp"
#include "ShipSimulator.hpp"
#include "SimClock.hpp"
#include "components/BeamScheduler.hpp"
#include "components/CalibrationMonitor.hpp"
#include "components/CommandHandler.hpp"
#include "components/DetectionProcessor.hpp"
#include "components/TrackManager.hpp"
#include "ui/UiApp.hpp"

int main(int argc, char** argv) {
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

    // Order matters only for startup cosmetics; DDS discovery handles wiring.
    ship.start();
    scheduler.start();
    processor.start();
    tracker.start();
    calibration.start();
    commands.start();
    console.start();

    radar::ui::UiApp app(bus, console);
    const int rc = app.run(); // blocks until the window closes

    std::cout << "[radar_app] shutting down\n";
    console.stop();
    commands.stop();
    calibration.stop();
    tracker.stop();
    processor.stop();
    scheduler.stop();
    ship.stop();
    return rc;
}
