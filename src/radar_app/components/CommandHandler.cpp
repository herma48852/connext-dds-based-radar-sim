#include "CommandHandler.hpp"

#include <cstdlib>
#include <cmath>
#include <iostream>

#include <dds/core/cond/WaitSet.hpp>
#include <dds/core/cond/StatusCondition.hpp>
#include "Log.hpp"
#include "RadarFaces.hpp"
#include "SearchRaster.hpp"
#include "SimClock.hpp"

namespace radar::app {

static_assert(types::FACE_FORWARD_STARBOARD == faces::kForwardStarboard);
static_assert(types::FACE_AFT_STARBOARD == faces::kAftStarboard);
static_assert(types::FACE_AFT_PORT == faces::kAftPort);
static_assert(types::FACE_FORWARD_PORT == faces::kForwardPort);
static_assert(types::FACE_COUNT == faces::kFaceCount);
static_assert(types::FACE_MASK_FORWARD_STARBOARD
              == static_cast<int32_t>(faces::kForwardStarboardMask));
static_assert(types::FACE_MASK_AFT_STARBOARD
              == static_cast<int32_t>(faces::kAftStarboardMask));
static_assert(types::FACE_MASK_AFT_PORT
              == static_cast<int32_t>(faces::kAftPortMask));
static_assert(types::FACE_MASK_FORWARD_PORT
              == static_cast<int32_t>(faces::kForwardPortMask));
static_assert(types::FACE_MASK_ALL
              == static_cast<int32_t>(faces::kAllFacesMask));

namespace {
double wrap180(double angle_deg) {
    while (angle_deg > 180.0) angle_deg -= 360.0;
    while (angle_deg < -180.0) angle_deg += 360.0;
    return angle_deg;
}
} // namespace

void CommandHandler::start() {
    auto topic = radds::make_topic<types::SystemCommand>(
        participant_, dds_names::TOPIC_SYSTEM_COMMAND);
    reader_ = radds::make_reader<types::SystemCommand>(
        subscriber_, topic, dds_names::PROFILE_SYSTEM_COMMAND);

    spawn([this] {
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
    });
}

void CommandHandler::dispatch(const types::SystemCommand& cmd) {
    const auto target_face_mask = faces::normalize_target_mask(
        static_cast<uint32_t>(cmd.target_face_mask));
    RADAR_LOG << "[CommandHandler] t=" << SimClock::sim_millis()
              << "ms command=" << static_cast<int>(cmd.command_type)
              << " face_mask=0x" << std::hex << target_face_mask << std::dec
              << " params=\"" << cmd.parameters << "\"\n";

    const auto for_each_targeted_face = [&](auto&& action) {
        for (const auto& face : faces::kDefinitions) {
            if ((target_face_mask & face.mask) != 0u)
                action(face, static_cast<std::size_t>(face.id));
        }
    };

    switch (cmd.command_type) {
    case types::CommandType::CMD_SET_MODE:
        // parameters: "search" | "sector"
        if (cmd.parameters == "sector") {
            for_each_targeted_face(
                [&](const auto&, std::size_t i) {
                    bus_.radar_mode[i].store(1);
                });
        } else if (cmd.parameters == "search") {
            for_each_targeted_face(
                [&](const auto&, std::size_t i) {
                    bus_.radar_mode[i].store(0);
                });
        } else {
            RADAR_LOG << "[CommandHandler] bad radar mode \""
                      << cmd.parameters << "\" (want search or sector)\n";
        }
        break;
    case types::CommandType::CMD_SET_SECTOR: {
        if (!std::isfinite(cmd.sector_center_deg) ||
            !std::isfinite(cmd.sector_width_deg) ||
            std::fabs(
                cmd.sector_width_deg - search_raster::kSectorWidthDeg)
                > 1.0e-6) {
            RADAR_LOG << "[CommandHandler] bad sector center="
                      << cmd.sector_center_deg << " width="
                      << cmd.sector_width_deg
                      << " (want finite center and fixed 30 deg width)\n";
            break;
        }
        double center = std::fmod(cmd.sector_center_deg, 360.0);
        if (center < 0.0)
            center += 360.0;
        for_each_targeted_face(
            [&](const auto& face, std::size_t i) {
                const double local_center =
                    wrap180(center - face.boresight_deg);
                if (std::fabs(local_center) + 0.5 * cmd.sector_width_deg
                    > 45.0) {
                    RADAR_LOG
                        << "[CommandHandler] sector center=" << center
                        << " width=" << cmd.sector_width_deg
                        << " crosses " << face.short_name
                        << " face limits; face ignored\n";
                    return;
                }
                bus_.sector_center_deg[i].store(center);
                bus_.sector_width_deg[i].store(cmd.sector_width_deg);
                bus_.radar_mode[i].store(1);
            });
        break;
    }
    case types::CommandType::CMD_SELF_TEST:
        bus_.self_test_requested.store(true);
        break;
    case types::CommandType::CMD_RESET:
        bus_.reset_requested.store(true);
        for (auto& mode : bus_.radar_mode)
            mode.store(0);
        break;
    case types::CommandType::CMD_DEGRADE_ARRAY:
        for_each_targeted_face(
            [&](const auto&, std::size_t i) {
                bus_.degrade_array[i].store(true);
            });
        break;
    case types::CommandType::CMD_RESTORE_ARRAY:
        for_each_targeted_face(
            [&](const auto&, std::size_t i) {
                bus_.degrade_array[i].store(false);
            });
        break;
    case types::CommandType::CMD_RMA_OFFLINE:
    case types::CommandType::CMD_RMA_ONLINE: {
        // parameters: RMA index "0".."15" or "all"
        const bool online = cmd.command_type == types::CommandType::CMD_RMA_ONLINE;
        long rma_index = -1;
        if (cmd.parameters != "all") {
            char* end = nullptr;
            rma_index = std::strtol(cmd.parameters.c_str(), &end, 10);
            if (end == cmd.parameters.c_str() || *end != '\0' ||
                rma_index < 0 || rma_index >= 16) {
                RADAR_LOG << "[CommandHandler] bad RMA index \""
                          << cmd.parameters << "\" (want 0..15 or all)\n";
                break;
            }
        }
        for_each_targeted_face(
            [&](const auto&, std::size_t i) {
                uint32_t mask = bus_.rma_offline_mask[i].load();
                if (cmd.parameters == "all") {
                    mask = online ? 0u : 0xFFFFu;
                } else if (online) {
                    mask &= ~(1u << rma_index);
                } else {
                    mask |= (1u << rma_index);
                }
                bus_.rma_offline_mask[i].store(mask);
            });
        break;
    }
    default:
        break;
    }
}

} // namespace radar::app
