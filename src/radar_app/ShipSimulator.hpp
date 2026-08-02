#pragma once
// ShipSimulator: own-ship GPS/INS. Publishes ShipPosition at 10 Hz
// (source_id = 0). DetectionProcessor, TrackManager, and HmiUi consume this
// state through their own DDS readers; there is no in-process navigation
// shortcut.
//
// The target generator publishes the same motion as ground truth
// (source_id = 1) so both instances exist on the keyed Ship/ShipPosition
// topic for correlation in Connext Studio.

#include "components/ComponentBase.hpp"

namespace radar::app {

class ShipSimulator : public ComponentBase {
public:
    explicit ShipSimulator(int32_t domain_id)
        : ComponentBase(domain_id, "Radar.ShipINS") {}

    ~ShipSimulator() override { stop(); }

    void start() override;

private:
    // Initial state: Atlantic, off the Virginia Capes
    static constexpr double kStartLatDeg  = 36.90;
    static constexpr double kStartLonDeg  = -75.90;
    static constexpr double kHeadingDeg   = 45.0;
    static constexpr double kSpeedMps     = 10.3;  // ~20 kn

    dds::pub::DataWriter<types::ShipPosition> writer_{dds::core::null};
};

} // namespace radar::app
