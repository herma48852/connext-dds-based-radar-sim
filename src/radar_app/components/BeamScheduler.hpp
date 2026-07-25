#pragma once
// BeamScheduler: publishes BeamCommand at 100 Hz.
//  - Search mode: continuous 360 deg rotation (full revolution ~1.6 s)
//  - Sector mode: back-and-forth scan inside the commanded sector
// Sector/mode changes arrive via SystemCommand -> CommandHandler -> DataBus.

#include "ComponentBase.hpp"
#include "SearchRaster.hpp"
#include "../DataBus.hpp"

namespace radar::app {

class BeamScheduler : public ComponentBase {
public:
    BeamScheduler(int32_t domain_id, DataBus& bus)
        : ComponentBase(domain_id, "Radar.BeamScheduler"), bus_(bus) {}

    ~BeamScheduler() override { stop(); }

    void start() override;

private:
    DataBus& bus_;
    dds::pub::DataWriter<types::BeamCommand> writer_{dds::core::null};
};

} // namespace radar::app
