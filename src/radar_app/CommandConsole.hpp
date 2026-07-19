#pragma once
// CommandConsole: lets the radar UI issue SystemCommands without making
// DDS calls on the render thread. UI pushes a request into a lock-free
// queue; a dedicated thread performs the actual DDS write.

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include "DdsSupport.hpp"
#include "SimClock.hpp"
#include "SpscQueue.hpp"
#include "TopicNames.hpp"

namespace radar::app {

class CommandConsole {
public:
    explicit CommandConsole(int32_t domain_id)
        : participant_(radds::make_participant(
              domain_id, dds_names::PROFILE_RADAR_PARTICIPANT, "Radar.CommandConsole")),
          publisher_(participant_) {
        auto topic = radds::make_topic<types::SystemCommand>(
            participant_, dds_names::TOPIC_SYSTEM_COMMAND);
        writer_ = radds::make_writer<types::SystemCommand>(
            publisher_, topic, dds_names::PROFILE_SYSTEM_COMMAND);
    }

    ~CommandConsole() { stop(); }

    void start() {
        thread_ = std::thread([this] {
            while (!stop_.load()) {
                CmdReq req;
                bool any = false;
                while (queue_.pop(req)) {
                    any = true;
                    types::SystemCommand cmd;
                    // Bounded key space: an ever-incrementing command id
                    // would leak one DDS instance per command.
                    cmd.command_id        = 100 + (cmd_id_++ % 128);
                    cmd.timestamp         = SimClock::stamp();
                    cmd.command_type      = static_cast<types::CommandType>(req.type);
                    cmd.sector_center_deg = req.center;
                    cmd.sector_width_deg  = req.width;
                    cmd.priority          = req.priority;
                    cmd.parameters        = req.params;
                    writer_.write(cmd);
                }
                if (!any) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
    }

    void stop() {
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
    }

    // Render-thread safe: never touches DDS.
    void send(int32_t type, double center = 0.0, double width = 0.0,
              const char* params = "", int32_t priority = 3) {
        CmdReq req{};
        req.type = type;
        req.center = center;
        req.width = width;
        req.priority = priority;
        std::strncpy(req.params, params, sizeof(req.params) - 1);
        queue_.push_overwrite(req);
    }

private:
    struct CmdReq {           // trivially copyable for the SPSC queue
        int32_t  type;
        double   center;
        double   width;
        int32_t  priority;
        char     params[128];
    };

    dds::domain::DomainParticipant participant_;
    dds::pub::Publisher            publisher_;
    dds::pub::DataWriter<types::SystemCommand> writer_{dds::core::null};
    SpscQueue<CmdReq>              queue_{64};
    std::thread                    thread_;
    std::atomic<bool>              stop_{false};
    int64_t                        cmd_id_{1};
};

} // namespace radar::app
