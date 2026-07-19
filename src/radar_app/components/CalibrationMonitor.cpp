#include "CalibrationMonitor.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "SimClock.hpp"

namespace radar::app {

void CalibrationMonitor::start() {
    auto topic = radds::make_topic<types::CalibrationStatus>(
        participant_, dds_names::TOPIC_CALIBRATION_STATUS);
    writer_ = radds::make_writer<types::CalibrationStatus>(
        publisher_, topic, dds_names::PROFILE_CALIBRATION_STATUS);

    spawn([this] {
        using namespace std::chrono;
        std::normal_distribution<float>  drift(0.0f, 0.15f);      // dB, benign
        std::normal_distribution<double> temp_walk(0.0, 0.05);
        auto next = steady_clock::now();

        types::CalibrationStatus msg;
        msg.array_id = 0;
        msg.element_drift_db.resize(types::MAX_ARRAY_ELEMENTS);

        while (!stop_.load()) {
            next += seconds(1); // 1 Hz

            temperature_c_ += temp_walk(rng_);
            temperature_c_ = std::clamp(temperature_c_, 35.0, 55.0);

            const bool degraded = bus_.degrade_array.load();
            int failed = 0;
            double drift_sum = 0.0;

            for (int i = 0; i < types::MAX_ARRAY_ELEMENTS; ++i) {
                float d = drift(rng_);
                if (degraded) {
                    // ~12% of elements fail hard, seeded deterministically
                    // per element so the failure pattern is stable on screen
                    if ((static_cast<unsigned>(i) * 2654435761u) % 100 < 12) {
                        d = -6.0f - std::fabs(drift(rng_)) * 4.0f;
                        ++failed;
                    }
                }
                msg.element_drift_db[i] = d;
                drift_sum += std::fabs(d);
            }

            const auto status =
                degraded ? (failed > 200 ? types::ArrayHealth::ARRAY_CRITICAL
                                         : types::ArrayHealth::ARRAY_DEGRADED)
                         : types::ArrayHealth::ARRAY_NOMINAL;

            msg.timestamp            = SimClock::stamp();
            msg.temperature_c        = temperature_c_;
            msg.failed_element_count = failed;
            msg.overall_status       = status;
            writer_.write(msg);

            bus_.update_health(HealthView{
                static_cast<int32_t>(status), failed,
                types::MAX_ARRAY_ELEMENTS, temperature_c_,
                drift_sum / types::MAX_ARRAY_ELEMENTS,
                SimClock::sim_millis()});

            std::this_thread::sleep_until(next);
        }
    });
}

} // namespace radar::app
