#include "TargetControlClient.hpp"

#include <algorithm>
#include <chrono>
#include <random>
#include <utility>

#include "SimClock.hpp"
#include "WorkerGuard.hpp"

namespace target_control {

TargetControlClient::TargetControlClient(int32_t control_domain_id)
    : control_domain_id_(control_domain_id),
      client_id_(static_cast<int32_t>(
          std::random_device{}() & 0x7FFFFFFFu)),
      participant_(radds::make_participant(
          control_domain_id,
          radar::dds_names::PROFILE_TARGET_CONTROL_PARTICIPANT,
          "TargetControl.UI")),
      publisher_(participant_),
      subscriber_(participant_) {
    auto request_topic =
        radds::make_topic<radar::types::TargetControlRequest>(
            participant_, radar::dds_names::TOPIC_TARGET_CONTROL_REQUEST);
    auto reply_topic =
        radds::make_topic<radar::types::TargetControlReply>(
            participant_, radar::dds_names::TOPIC_TARGET_CONTROL_REPLY);
    auto snapshot_topic =
        radds::make_topic<radar::types::TargetControlSnapshot>(
            participant_, radar::dds_names::TOPIC_TARGET_CONTROL_SNAPSHOT);
    request_writer_ =
        radds::make_writer<radar::types::TargetControlRequest>(
            publisher_, request_topic,
            radar::dds_names::PROFILE_TARGET_CONTROL_REQUEST);
    reply_reader_ =
        radds::make_reader<radar::types::TargetControlReply>(
            subscriber_, reply_topic,
            radar::dds_names::PROFILE_TARGET_CONTROL_REPLY);
    snapshot_reader_ =
        radds::make_reader<radar::types::TargetControlSnapshot>(
            subscriber_, snapshot_topic,
            radar::dds_names::PROFILE_TARGET_CONTROL_SNAPSHOT);
}

void TargetControlClient::start() {
    stop_.store(false);
    thread_ = std::thread([this] {
        radar::run_worker_guarded(
            "TargetControl.DdsClient", [this] { loop(); });
    });
}

void TargetControlClient::stop() {
    stop_.store(true);
    if (thread_.joinable())
        thread_.join();
}

bool TargetControlClient::connected() const {
    std::lock_guard lock(state_mutex_);
    return snapshot_.has_value() &&
           std::chrono::steady_clock::now() - last_snapshot_ <
               std::chrono::seconds(2);
}

std::optional<radar::types::TargetControlSnapshot>
TargetControlClient::snapshot() const {
    std::lock_guard lock(state_mutex_);
    return snapshot_;
}

std::vector<radar::types::TargetControlReply>
TargetControlClient::take_replies() {
    std::lock_guard lock(state_mutex_);
    std::vector<radar::types::TargetControlReply> result;
    result.swap(replies_);
    return result;
}

int64_t TargetControlClient::enqueue(
        radar::types::TargetControlAction action,
        const std::string& template_name,
        int64_t scenario_instance_id,
        int32_t target_id,
        int32_t target_count) {
    radar::types::TargetControlRequest request;
    request.client_id = client_id_;
    request.request_id = next_request_id_.fetch_add(1);
    request.timestamp = radar::SimClock::stamp();
    request.action = action;
    request.scenario_template = template_name;
    request.scenario_instance_id = scenario_instance_id;
    request.target_id = target_id;
    request.target_count = target_count;
    {
        std::lock_guard lock(request_mutex_);
        requests_.push_back(request);
    }
    return request.request_id;
}

int64_t TargetControlClient::add_scenario(
        const std::string& template_name, int32_t target_count) {
    return enqueue(
        radar::types::TargetControlAction::TARGET_CONTROL_ADD_SCENARIO,
        template_name, 0, 0, target_count);
}

int64_t TargetControlClient::remove_scenario(
        int64_t scenario_instance_id) {
    return enqueue(
        radar::types::TargetControlAction::TARGET_CONTROL_REMOVE_SCENARIO,
        {}, scenario_instance_id);
}

int64_t TargetControlClient::remove_target(int32_t target_id) {
    return enqueue(
        radar::types::TargetControlAction::TARGET_CONTROL_REMOVE_TARGET,
        {}, 0, target_id);
}

int64_t TargetControlClient::clear_all() {
    return enqueue(
        radar::types::TargetControlAction::TARGET_CONTROL_CLEAR_ALL);
}

void TargetControlClient::loop() {
    using namespace std::chrono_literals;
    while (!stop_.load()) {
        std::deque<radar::types::TargetControlRequest> pending;
        {
            std::lock_guard lock(request_mutex_);
            pending.swap(requests_);
        }
        for (const auto& request : pending)
            request_writer_.write(request);

        radar::types::TargetControlReply reply;
        dds::sub::SampleInfo reply_info;
        for (int i = 0;
             i < 64 && reply_reader_.extensions().take(
                 reply, reply_info); ++i) {
            if (!reply_info.valid() || reply.client_id != client_id_)
                continue;
            std::lock_guard lock(state_mutex_);
            replies_.push_back(reply);
            if (replies_.size() > 64)
                replies_.erase(replies_.begin());
        }

        radar::types::TargetControlSnapshot snapshot;
        dds::sub::SampleInfo snapshot_info;
        bool received_snapshot = false;
        while (snapshot_reader_.extensions().take(
                   snapshot, snapshot_info)) {
            if (snapshot_info.valid())
                received_snapshot = true;
        }
        if (received_snapshot) {
            std::lock_guard lock(state_mutex_);
            snapshot_ = std::move(snapshot);
            last_snapshot_ = std::chrono::steady_clock::now();
        }

        std::this_thread::sleep_for(10ms);
    }
}

} // namespace target_control
