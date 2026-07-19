#pragma once
// CalibrationMonitor: publishes CalibrationStatus at 1 Hz.
//
// Models per-element (T/R module) gain drift plus a slow temperature random
// walk. When the CommandHandler sets the DEGRADE flag (demo scenario), a
// fraction of elements is marked failed and overall_status drops to
// ARRAY_DEGRADED - visible both in the health panel and in Connext Studio.

#include <random>

#include "ComponentBase.hpp"
#include "../DataBus.hpp"

namespace radar::app {

class CalibrationMonitor : public ComponentBase {
public:
    CalibrationMonitor(int32_t domain_id, DataBus& bus)
        : ComponentBase(domain_id, "Radar.CalibrationMonitor"), bus_(bus) {}

    void start() override;

private:
    DataBus& bus_;
    dds::pub::DataWriter<types::CalibrationStatus> writer_{dds::core::null};

    std::mt19937 rng_{std::random_device{}()};
    double temperature_c_{42.0};
};

} // namespace radar::app
