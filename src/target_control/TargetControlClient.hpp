#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "DdsSupport.hpp"
#include "TopicNames.hpp"

namespace target_control {

class TargetControlClient {
public:
    explicit TargetControlClient(int32_t control_domain_id);
    ~TargetControlClient() { stop(); }

    void start();
    void stop();

    int32_t domain_id() const { return control_domain_id_; }
    bool connected() const;
    std::optional<radar::types::TargetControlSnapshot> snapshot() const;
    std::vector<radar::types::TargetControlReply> take_replies();

    int64_t add_scenario(const std::string& template_name,
                         int32_t target_count);
    int64_t remove_scenario(int64_t scenario_instance_id);
    int64_t remove_target(int32_t target_id);
    int64_t clear_all();

private:
    int64_t enqueue(radar::types::TargetControlAction action,
                    const std::string& template_name = {},
                    int64_t scenario_instance_id = 0,
                    int32_t target_id = 0,
                    int32_t target_count = 0);
    void loop();

    int32_t control_domain_id_;
    int32_t client_id_;
    std::atomic<int64_t> next_request_id_{1};

    dds::domain::DomainParticipant participant_;
    dds::pub::Publisher publisher_;
    dds::sub::Subscriber subscriber_;
    dds::pub::DataWriter<radar::types::TargetControlRequest>
        request_writer_{dds::core::null};
    dds::sub::DataReader<radar::types::TargetControlReply>
        reply_reader_{dds::core::null};
    dds::sub::DataReader<radar::types::TargetControlSnapshot>
        snapshot_reader_{dds::core::null};

    mutable std::mutex state_mutex_;
    std::optional<radar::types::TargetControlSnapshot> snapshot_;
    std::chrono::steady_clock::time_point last_snapshot_{};
    std::vector<radar::types::TargetControlReply> replies_;

    std::mutex request_mutex_;
    std::deque<radar::types::TargetControlRequest> requests_;

    std::thread thread_;
    std::atomic<bool> stop_{false};
};

} // namespace target_control
