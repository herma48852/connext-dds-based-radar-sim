#pragma once

#include <cstdint>

namespace radar::app {

// DDS-free source identity carried through resolution-cell fusion and tracker
// association. The DDS adapter converts this to
// AssociationDetectionIdentity at publication time.
struct DetectionIdentity {
    int32_t sensor_id{-1};
    int32_t detection_id{-1};
    int32_t beam_id{-1};
    int64_t epoch_millis{0};
    int64_t sim_millis{0};
};

} // namespace radar::app
