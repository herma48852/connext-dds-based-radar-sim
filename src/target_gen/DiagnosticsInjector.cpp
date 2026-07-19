#include "DiagnosticsInjector.hpp"

#include <chrono>
#include <iostream>
#include <utility>

#include "SimClock.hpp"

namespace target_gen {

using namespace radar;          // types::
namespace dn = radar::dds_names;

void DiagnosticsInjector::inject_qos_mismatch() {
    std::cout << "[target_gen] Injecting QoS mismatch: RELIABLE reader on "
              << dn::TOPIC_DETECTION_EVENT << " (writers are BEST_EFFORT)\n";

    rogue_reader_part_ = radds::make_participant(
        domain_id_, dn::PROFILE_TARGETGEN_PARTICIPANT, "TargetGen.RogueReader");
    dds::sub::Subscriber sub(rogue_reader_part_);
    auto topic = radds::make_topic<types::DetectionEvent>(
        rogue_reader_part_, dn::TOPIC_DETECTION_EVENT);

    // Start from the file profile, then deliberately break compatibility.
    auto rqos = radds::qos_provider().datareader_qos(dn::PROFILE_DETECTION_EVENT);
    rqos << dds::core::policy::Reliability::Reliable();
    dds::sub::DataReader<types::DetectionEvent> reader(sub, topic, rqos);

    // Keep the reader alive; discovery reports the mismatch continuously.
    std::thread([r = std::move(reader)] {
        // no WaitSet needed: the entity just exists to be diagnosed
        while (true) std::this_thread::sleep_for(std::chrono::hours(24));
    }).detach();
}

void DiagnosticsInjector::inject_type_mismatch() {
    std::cout << "[target_gen] Injecting type mismatch: DetectionEvent writer "
                 "on topic name \"" << dn::TOPIC_TARGET_TRUTH << "\"\n";

    rogue_writer_part_ = radds::make_participant(
        domain_id_, dn::PROFILE_TARGETGEN_PARTICIPANT, "TargetGen.RogueWriter");
    dds::pub::Publisher pub(rogue_writer_part_);

    // Same topic NAME as TargetTruth, different registered TYPE -> Studio
    // reports an inconsistent topic / type conflict.
    auto wrong_topic = dds::topic::Topic<types::DetectionEvent>(
        rogue_writer_part_, dn::TOPIC_TARGET_TRUTH);
    rogue_writer_ = radds::make_writer<types::DetectionEvent>(
        pub, wrong_topic, dn::PROFILE_DETECTION_EVENT);

    mismatch_writer_thread_ = std::thread([this] {
        while (!stop_.load()) {
            types::DetectionEvent det;
            det.sensor_id     = 0; // constant key (matches IDL @key)
            det.detection_id  = -1;
            det.timestamp     = SimClock::stamp();
            det.range_m       = 0.0;
            det.azimuth_deg   = 0.0;
            det.elevation_deg = 0.0;
            det.amplitude     = 0.0;
            det.snr_db        = 0.0;
            rogue_writer_.write(det);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    });
}

void DiagnosticsInjector::send_degrade_command() {
    std::cout << "[target_gen] Sending CMD_DEGRADE_ARRAY\n";

    commander_part_ = radds::make_participant(
        domain_id_, dn::PROFILE_TARGETGEN_PARTICIPANT, "TargetGen.Commander");
    dds::pub::Publisher pub(commander_part_);
    auto topic = radds::make_topic<types::SystemCommand>(
        commander_part_, dn::TOPIC_SYSTEM_COMMAND);
    auto writer = radds::make_writer<types::SystemCommand>(
        pub, topic, dn::PROFILE_SYSTEM_COMMAND);

    // Give discovery a moment, then send the command (twice for safety;
    // RELIABLE + same command_id is idempotent enough for the demo).
    std::this_thread::sleep_for(std::chrono::seconds(2));
    for (int i = 0; i < 2; ++i) {
        types::SystemCommand cmd;
        cmd.command_id   = 9000 + i;
        cmd.timestamp    = SimClock::stamp();
        cmd.command_type = types::CommandType::CMD_DEGRADE_ARRAY;
        cmd.priority     = 1;
        cmd.parameters   = "scenario=degraded_array";
        writer.write(cmd);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

void DiagnosticsInjector::stop() {
    stop_.store(true);
    if (mismatch_writer_thread_.joinable())
        mismatch_writer_thread_.join();
}

} // namespace target_gen
