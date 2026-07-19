#pragma once
// CommandHandler: the WaitSet-based command path.
//
//   subscribes: Radar/SystemCommand (RELIABLE, via WaitSet + ReadCondition)
//
// Commands are lower-rate but must be handled atomically and in order, so
// instead of a listener we use a dedicated thread blocking on a WaitSet.
// Dispatched effects are published to other components through the atomic
// command state in the DataBus (no DDS loops for internal dispatch).

#include "ComponentBase.hpp"
#include "../DataBus.hpp"

namespace radar::app {

class CommandHandler : public ComponentBase {
public:
    CommandHandler(int32_t domain_id, DataBus& bus)
        : ComponentBase(domain_id, "Radar.CommandHandler"), bus_(bus) {}

    void start() override;

private:
    void dispatch(const types::SystemCommand& cmd);

    DataBus& bus_;
    dds::sub::DataReader<types::SystemCommand> reader_{dds::core::null};
};

} // namespace radar::app
