#pragma once
// BeamScheduler: publishes BeamCommand at 50 Hz.
//  - Search mode: continuous 360 deg rotation (full revolution ~3.2 s)
//  - Sector mode: back-and-forth scan inside the commanded sector
// Sector/mode changes arrive via SystemCommand -> CommandHandler -> DataBus.

#include "ComponentBase.hpp"
#include "../DataBus.hpp"

namespace radar::app {

class BeamScheduler : public ComponentBase {
public:
    BeamScheduler(int32_t domain_id, DataBus& bus)
        : ComponentBase(domain_id, "Radar.BeamScheduler"), bus_(bus) {}

    void start() override;

private:
    static constexpr double kDwellPeriodSec = 0.02;  // 50 Hz
    static constexpr double kAzStepDeg      = 2.25;  // 160 dwells / revolution

    DataBus& bus_;
    dds::pub::DataWriter<types::BeamCommand> writer_{dds::core::null};
};

} // namespace radar::app
