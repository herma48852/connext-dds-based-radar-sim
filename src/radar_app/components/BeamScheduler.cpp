#include "BeamScheduler.hpp"

#include <chrono>
#include <cmath>

#include "PeriodicDeadline.hpp"
#include "SimClock.hpp"

namespace radar::app {

namespace {
double wrap360(double a) {
    while (a >= 360.0) a -= 360.0;
    while (a < 0.0)     a += 360.0;
    return a;
}
// Three-bar elevation raster: with the +/-5.5 deg elevation gate in
// DetectionProcessor these tile without overlap, covering the deck up to
// ~30.5 deg (ship / fighters / bomber / decoy / drone). The bar advances
// once per azimuth revolution (or per sector bounce). Revisit per bar:
// 3 x 1.6 s = 4.8 s < 9 s coast. Targets above ~30.5 deg at close range
// stay outside the surveillance cone ("cone of silence").
constexpr int kNumElBars = 3;
constexpr double kElBarsDeg[kNumElBars] = {3.0, 14.0, 25.0};
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
        int el_bar = 0; // index into kElBarsDeg
        auto next = std::chrono::steady_clock::now();

        while (!stop_.load()) {
            next = advance_periodic_deadline(
                next,
                std::chrono::milliseconds(
                    static_cast<int>(kDwellPeriodSec * 1000.0))); // 100 Hz

            const int32_t mode = bus_.radar_mode.load();
            types::BeamCommand cmd;
            cmd.scheduler_id = 0; // constant key: one DDS instance
            cmd.beam_id   = beam_id;
            cmd.timestamp = SimClock::stamp();

            if (mode == 1) {
                // Sector scan: sweep back and forth inside [center-w/2, center+w/2]
                const double center = bus_.sector_center_deg.load();
                const double width  = bus_.sector_width_deg.load();
                const double lo = center - width * 0.5;
                const double hi = center + width * 0.5;
                az += sector_dir * kAzStepDeg;
                if (az > hi) { az = hi; sector_dir = -1; el_bar = (el_bar + 1) % kNumElBars; }
                if (az < lo) { az = lo; sector_dir = +1; el_bar = (el_bar + 1) % kNumElBars; }
                cmd.mode     = types::BeamMode::BEAM_MODE_SEARCH;
                cmd.priority = 2;
            } else {
                if (az + kAzStepDeg >= 360.0)
                    el_bar = (el_bar + 1) % kNumElBars; // new revolution
                az = wrap360(az + kAzStepDeg);
                cmd.mode     = types::BeamMode::BEAM_MODE_SEARCH;
                cmd.priority = 3;
            }

            const double el_deg = kElBarsDeg[el_bar];
            cmd.azimuth_deg   = az;
            cmd.elevation_deg = el_deg;
            cmd.dwell_time_us = static_cast<int32_t>(kDwellPeriodSec * 1e6);
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
