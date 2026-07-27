#pragma once
// BeamScheduler: publishes one keyed BeamCommand per face every 10 ms
// (100 Hz/face, 400 Hz aggregate).
//  - Search mode: forty half-step-inset centers across each 90-degree face
//  - Sector mode: thirteen centers across the selected 30-degree sector
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
