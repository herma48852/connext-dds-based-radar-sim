#include "ShipSimulator.hpp"

#include <chrono>
#include <cmath>

#include "PeriodicDeadline.hpp"
#include "SimClock.hpp"

namespace radar::app {

void ShipSimulator::start() {
    auto topic = radds::make_topic<types::ShipPosition>(
        participant_, dds_names::TOPIC_SHIP_POSITION);
    writer_ = radds::make_writer<types::ShipPosition>(
        publisher_, topic, dds_names::PROFILE_SHIP_POSITION);

    spawn([this] {
        using namespace std::chrono;
        constexpr double kDeg2Rad = 3.14159265358979323846 / 180.0;
        constexpr double kMetersPerDegLat = 111320.0;

        double lat = kStartLatDeg, lon = kStartLonDeg;
        auto next = steady_clock::now();

        while (!stop_.load()) {
            next = advance_periodic_deadline(
                next, milliseconds(100)); // 10 Hz INS

            const double t = SimClock::sim_millis() / 1000.0;
            // gentle seaway motion
            const double pitch = 0.8 * std::sin(t * 0.50);
            const double roll  = 2.5 * std::sin(t * 0.31 + 1.2);

            // integrate course over ground
            const double dt = 0.1;
            const double d_north = kSpeedMps * std::cos(kHeadingDeg * kDeg2Rad) * dt;
            const double d_east  = kSpeedMps * std::sin(kHeadingDeg * kDeg2Rad) * dt;
            lat += d_north / kMetersPerDegLat;
            lon += d_east / (kMetersPerDegLat * std::cos(lat * kDeg2Rad));

            types::ShipPosition msg;
            msg.source_id     = 0;
            msg.timestamp     = SimClock::stamp();
            msg.latitude_deg  = lat;
            msg.longitude_deg = lon;
            msg.altitude_m    = 0.0;
            msg.heading_deg   = kHeadingDeg;
            msg.course_deg    = kHeadingDeg;
            msg.speed_mps     = kSpeedMps;
            msg.pitch_deg     = pitch;
            msg.roll_deg      = roll;
            writer_.write(msg);

            bus_.update_ship(ShipView{
                lat, lon, 0.0, kHeadingDeg, kHeadingDeg, kSpeedMps,
                pitch, roll, SimClock::sim_millis()});

            std::this_thread::sleep_until(next);
        }
    });
}

} // namespace radar::app
