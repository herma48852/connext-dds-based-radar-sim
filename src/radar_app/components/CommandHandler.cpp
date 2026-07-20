#include "CommandHandler.hpp"

#include <cstdlib>
#include <iostream>

#include <dds/core/cond/WaitSet.hpp>
#include <dds/sub/cond/ReadCondition.hpp>
#include <dds/sub/status/DataState.hpp>

#include "Log.hpp"
#include "SimClock.hpp"

namespace radar::app {

void CommandHandler::start() {
    auto topic = radds::make_topic<types::SystemCommand>(
        participant_, dds_names::TOPIC_SYSTEM_COMMAND);
    reader_ = radds::make_reader<types::SystemCommand>(
        subscriber_, topic, dds_names::PROFILE_SYSTEM_COMMAND);

    spawn([this] {
        // WaitSet pattern: block until a SystemCommand arrives (or 500 ms
        // timeout so we can notice the stop flag). Multiple conditions
        // (e.g. a second reader or status changes) can be attached here.
        dds::sub::cond::ReadCondition read_cond(
            reader_, dds::sub::status::DataState::new_data());
        dds::core::cond::WaitSet waitset;
        waitset.attach_condition(read_cond);

        while (!stop_.load()) {
            // Unblocks on trigger OR timeout; after a timeout take() simply
            // returns no samples, which is harmless.
            waitset.dispatch(dds::core::Duration::from_millisecs(500));

            auto samples = reader_.take();
            for (const auto& s : samples)
                if (s.info().valid())
                    dispatch(s.data());
        }
    });
}

void CommandHandler::dispatch(const types::SystemCommand& cmd) {
    RADAR_LOG << "[CommandHandler] t=" << SimClock::sim_millis()
              << "ms command=" << static_cast<int>(cmd.command_type)
              << " params=\"" << cmd.parameters << "\"\n";

    switch (cmd.command_type) {
    case types::CommandType::CMD_SET_MODE:
        // parameters: "search" | "sector"
        bus_.radar_mode.store(cmd.parameters == "sector" ? 1 : 0);
        break;
    case types::CommandType::CMD_SET_SECTOR:
        bus_.sector_center_deg.store(cmd.sector_center_deg);
        bus_.sector_width_deg.store(cmd.sector_width_deg);
        bus_.radar_mode.store(1);
        break;
    case types::CommandType::CMD_SELF_TEST:
        bus_.self_test_requested.store(true);
        break;
    case types::CommandType::CMD_RESET:
        bus_.reset_requested.store(true);
        bus_.radar_mode.store(0);
        break;
    case types::CommandType::CMD_DEGRADE_ARRAY:
        bus_.degrade_array.store(true);
        break;
    case types::CommandType::CMD_RESTORE_ARRAY:
        bus_.degrade_array.store(false);
        break;
    case types::CommandType::CMD_RMA_OFFLINE:
    case types::CommandType::CMD_RMA_ONLINE: {
        // parameters: RMA index "0".."15" or "all"
        const bool online = cmd.command_type == types::CommandType::CMD_RMA_ONLINE;
        uint32_t mask = bus_.rma_offline_mask.load();
        if (cmd.parameters == "all") {
            mask = online ? 0u : 0xFFFFu;
        } else {
            char* end = nullptr;
            const long idx = std::strtol(cmd.parameters.c_str(), &end, 10);
            if (end == cmd.parameters.c_str() || idx < 0 || idx >= 16) {
                RADAR_LOG << "[CommandHandler] bad RMA index \""
                          << cmd.parameters << "\" (want 0..15 or all)\n";
                break;
            }
            if (online) mask &= ~(1u << idx);
            else        mask |=  (1u << idx);
        }
        bus_.rma_offline_mask.store(mask);
        break;
    }
    default:
        break;
    }
}

} // namespace radar::app
