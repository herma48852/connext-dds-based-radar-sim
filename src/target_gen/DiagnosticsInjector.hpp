#pragma once
// DiagnosticsInjector: live diagnostic scenarios for the Connext Studio
// demo. Each injected entity gets its own named participant so it appears
// as a distinct node in Studio's topology map.
//
//   inject_qos_mismatch(): a RELIABLE DataReader on Radar/DetectionEvent
//       (whose writers are BEST_EFFORT). DDS discovery reports the
//       requested/offered incompatibility and Studio flags it.
//   inject_type_mismatch(): a DataWriter of type DetectionEvent on the
//       topic NAME "TargetGen/TargetTruth" (registered type = TargetTruth).
//       Discovery reports an inconsistent-topic / type conflict.
//   send_degrade_command(): one-shot SystemCommand(CMD_DEGRADE_ARRAY) so
//       the degraded-array scenario can be driven from this app too.

#include <atomic>
#include <string>
#include <thread>

#include "DdsSupport.hpp"
#include "TopicNames.hpp"

namespace target_gen {

class DiagnosticsInjector {
public:
    explicit DiagnosticsInjector(int32_t domain_id) : domain_id_(domain_id) {}
    ~DiagnosticsInjector() { stop(); }

    void inject_qos_mismatch();
    void inject_type_mismatch();
    void send_degrade_command();
    // One-shot SystemCommand(CMD_RMA_OFFLINE, params) — params is the RMA
    // index "0".."15" or "all". Scripted counterpart of the pane click.
    void send_rma_offline(const std::string& params);
    void stop();

private:
    void send_system_command(radar::types::CommandType type,
                             const std::string& params);

    int32_t domain_id_;
    std::atomic<bool> stop_{false};
    std::thread mismatch_writer_thread_;

    // Keep entities alive for the lifetime of the injector
    dds::domain::DomainParticipant rogue_reader_part_{dds::core::null};
    dds::domain::DomainParticipant rogue_writer_part_{dds::core::null};
    dds::domain::DomainParticipant commander_part_{dds::core::null};
    dds::pub::DataWriter<radar::types::DetectionEvent> rogue_writer_{dds::core::null};
};

} // namespace target_gen
