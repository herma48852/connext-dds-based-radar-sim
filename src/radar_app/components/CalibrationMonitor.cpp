#include "CalibrationMonitor.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>

#include "RadarFaces.hpp"
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
        auto next_heartbeat = steady_clock::now();
        std::array<uint32_t, faces::kFaceCount> last_rma_masks{};
        last_rma_masks.fill(0xFFFFFFFFu);
        std::array<bool, faces::kFaceCount> last_degraded{};
        for (std::size_t i = 0; i < last_degraded.size(); ++i)
            last_degraded[i] = !bus_.degrade_array[i].load();

        types::CalibrationStatus msg;
        msg.element_drift_db.resize(types::MAX_ARRAY_ELEMENTS);

        while (!stop_.load()) {
            const auto now = steady_clock::now();
            const bool heartbeat = now >= next_heartbeat;
            bool any_state_changed = false;
            for (std::size_t i = 0; i < faces::kFaceCount; ++i) {
                any_state_changed =
                    any_state_changed
                    || (bus_.rma_offline_mask[i].load() & 0xFFFFu)
                           != last_rma_masks[i]
                    || bus_.degrade_array[i].load() != last_degraded[i];
            }
            if (!any_state_changed && !heartbeat) {
                std::this_thread::sleep_for(milliseconds(20));
                continue;
            }

            if (heartbeat) {
                do {
                    next_heartbeat += seconds(1);
                } while (next_heartbeat <= now);
                for (auto& temperature : temperature_c_) {
                    temperature += temp_walk(rng_);
                    temperature = std::clamp(temperature, 35.0, 55.0);
                }
            }

            for (const auto& face : faces::kDefinitions) {
                const auto face_index =
                    static_cast<std::size_t>(face.id);
                const bool degraded =
                    bus_.degrade_array[face_index].load();
                const uint32_t rma_mask =
                    bus_.rma_offline_mask[face_index].load() & 0xFFFFu;
                const bool state_changed =
                    rma_mask != last_rma_masks[face_index]
                    || degraded != last_degraded[face_index];
                if (!state_changed && !heartbeat)
                    continue;

                const int rma_off = std::popcount(rma_mask);
                int failed =
                    64 * rma_off; // offline RMAs: 64 dark elements each

                for (int element = 0;
                     element < types::MAX_ARRAY_ELEMENTS; ++element) {
                    // element (32x32 row-major) -> RMA block (4x4 of 8x8)
                    const int rma =
                        (element / 256) * 4 + (element % 32) / 8;
                    float d;
                    if ((rma_mask >> rma) & 1u) {
                        d = -60.0f; // RMA offline: element dark
                    } else {
                        d = drift(rng_);
                        if (degraded &&
                            (static_cast<unsigned>(element)
                             * 2654435761u
                             + static_cast<unsigned>(face.id) * 97u)
                                % 100 < 12) {
                            d = -6.0f - std::fabs(drift(rng_)) * 4.0f;
                            ++failed;
                        }
                    }
                    msg.element_drift_db[element] = d;
                }

                const auto status =
                    rma_off == 16 ? types::ArrayHealth::ARRAY_OFFLINE
                  : (degraded || rma_off > 0)
                        ? (failed > 200
                               ? types::ArrayHealth::ARRAY_CRITICAL
                               : types::ArrayHealth::ARRAY_DEGRADED)
                        : types::ArrayHealth::ARRAY_NOMINAL;

                msg.array_id = face.id;
                msg.timestamp = SimClock::stamp();
                msg.temperature_c = temperature_c_[face_index];
                msg.failed_element_count = failed;
                msg.overall_status = status;
                msg.rma_offline_mask = static_cast<int32_t>(rma_mask);
                writer_.write(msg);
                last_rma_masks[face_index] = rma_mask;
                last_degraded[face_index] = degraded;
            }
        }
    });
}

} // namespace radar::app
