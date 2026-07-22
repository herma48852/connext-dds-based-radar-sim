#include "CommandHandler.hpp"

#include <cstdlib>
#include <iostream>

#include <dds/core/cond/WaitSet.hpp>
#include <dds/core/cond/StatusCondition.hpp>
#include "Log.hpp"
#include "SimClock.hpp"

namespace radar::app {

void CommandHandler::start() {
    auto topic = radds::make_topic<types::SystemCommand>(
        participant_, dds_names::TOPIC_SYSTEM_COMMAND);
    reader_ = radds::make_reader<types::SystemCommand>(
        subscriber_, topic, dds_names::PROFILE_SYSTEM_COMMAND);

    spawn([this] {
        try {
            dds::core::cond::StatusCondition condition(reader_);
            condition.enabled_statuses(
                dds::core::status::StatusMask::data_available());

            dds::core::cond::WaitSet waitset;
            waitset += condition;
            while (!stop_.load()) {
                const auto active = waitset.wait(
                    dds::core::Duration::from_millisecs(200));
                if (stop_.load() || active.empty())
                    continue;
                types::SystemCommand sample;
                dds::sub::SampleInfo info;
                for (int i = 0;
                     i < 64 && reader_.extensions().take(sample, info); ++i) {
                    if (info.valid())
                        dispatch(sample);
                }
            }
        } catch (const std::exception& e) {
            RADAR_LOG << "[CommandHandler] worker exception: "
                      << e.what() << "\n";
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
