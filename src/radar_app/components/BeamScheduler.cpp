#include "BeamScheduler.hpp"

#include <chrono>
#include <cmath>

#include "PeriodicDeadline.hpp"
#include "SimClock.hpp"

namespace radar::app {

namespace {
double wrap180(double a) {
    while (a > 180.0)  a -= 360.0;
    while (a < -180.0) a += 360.0;
    return a;
}
// Three-bar elevation raster: with the +/-5.5 deg elevation gate in
// DetectionProcessor these tile without overlap, covering the deck up to
// ~30.5 deg (ship / fighters / bomber / decoy / drone). The bar advances
// once per azimuth revolution (or per sector bounce). Revisit per bar:
// 3 x 1.6 s = 4.8 s < 12 s coast. Targets above ~30.5 deg at close range
// stay outside the surveillance cone ("cone of silence").
} // namespace

void BeamScheduler::start() {
    auto topic = radds::make_topic<types::BeamCommand>(
        participant_, dds_names::TOPIC_BEAM_COMMAND);
    writer_ = radds::make_writer<types::BeamCommand>(
        publisher_, topic, dds_names::PROFILE_BEAM_COMMAND);

    spawn([this] {
        int64_t beam_id = 0;
        double az = 0.0;
        int sector_dir = +1;
        int el_bar = 0;
        auto next = std::chrono::steady_clock::now();

        while (!stop_.load()) {
            next = advance_periodic_deadline(
                next,
                std::chrono::milliseconds(
                    static_cast<int>(
                        search_raster::kDwellPeriodSec * 1000.0))); // 100 Hz

            const int32_t mode = bus_.radar_mode.load();
            types::BeamCommand cmd;
            cmd.scheduler_id = 0; // constant key: one DDS instance
            cmd.beam_id   = beam_id;
            cmd.timestamp = SimClock::stamp();

            if (mode == 1) {
                // Scan in center-relative coordinates so a sector spanning
                // north (for example 350 +/- 30 deg) remains normalized.
                const double center = bus_.sector_center_deg.load();
                const double width  = bus_.sector_width_deg.load();
                const double half_width = width * 0.5;
                double relative = wrap180(az - center)
                                + sector_dir
                                    * search_raster::kAzimuthStepDeg;
                if (relative > half_width) {
                    relative = half_width;
                    sector_dir = -1;
                    el_bar = (el_bar + 1)
                           % static_cast<int>(
                               search_raster::kElevationBarsDeg.size());
                }
                if (relative < -half_width) {
                    relative = -half_width;
                    sector_dir = +1;
                    el_bar = (el_bar + 1)
                           % static_cast<int>(
                               search_raster::kElevationBarsDeg.size());
                }
                az = search_raster::wrap360(center + relative);
                cmd.mode     = types::BeamMode::BEAM_MODE_SEARCH;
                cmd.priority = 2;
            } else {
                const auto pointing = search_raster::advance(az, el_bar);
                az = pointing.azimuth_deg;
                cmd.mode     = types::BeamMode::BEAM_MODE_SEARCH;
                cmd.priority = 3;
            }

            const double el_deg =
                search_raster::kElevationBarsDeg[
                    static_cast<std::size_t>(el_bar)];
            cmd.azimuth_deg   = az;
            cmd.elevation_deg = el_deg;
            cmd.dwell_time_us = static_cast<int32_t>(
                search_raster::kDwellPeriodSec * 1e6);
            writer_.write(cmd);

            bus_.current_beam_az_deg.store(az);
            bus_.current_beam_el_deg.store(el_deg);
            bus_.beam_commands.push_overwrite(BeamView{
                beam_id, az, el_deg,
                static_cast<int32_t>(cmd.mode), cmd.priority,
                SimClock::sim_millis()});

            ++beam_id;
            std::this_thread::sleep_until(next);
        }
    });
}

} // namespace radar::app
