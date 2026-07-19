#pragma once
// ShipSimulator: own-ship GPS/INS. Publishes ShipPosition at 10 Hz
// (source_id = 0) and mirrors it into the DataBus for the UI and for the
// DetectionProcessor's coordinate conversion.
//
// The target generator publishes the same motion as ground truth
// (source_id = 1) so both instances exist on the keyed Ship/ShipPosition
// topic for correlation in Connext Studio.

#include "components/ComponentBase.hpp"
#include "DataBus.hpp"

namespace radar::app {

class ShipSimulator : public ComponentBase {
public:
    ShipSimulator(int32_t domain_id, DataBus& bus)
        : ComponentBase(domain_id, "Radar.ShipINS"), bus_(bus) {}

    void start() override;

private:
    // Initial state: Atlantic, off the Virginia Capes
    static constexpr double kStartLatDeg  = 36.90;
    static constexpr double kStartLonDeg  = -75.90;
    static constexpr double kHeadingDeg   = 45.0;
    static constexpr double kSpeedMps     = 10.3;  // ~20 kn

    DataBus& bus_;
    dds::pub::DataWriter<types::ShipPosition> writer_{dds::core::null};
};

} // namespace radar::app
