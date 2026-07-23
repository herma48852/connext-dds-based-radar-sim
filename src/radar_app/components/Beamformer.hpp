#pragma once
// Beamformer: converts steering intent plus array health into the effective
// outage-aware beam response.
//
//   subscribes: Radar/BeamCommand        (commanded pointing direction)
//   subscribes: Radar/CalibrationStatus  (RMA outage state)
//   publishes : Radar/BeamPatternStatus  (20 Hz effective azimuth response)

#include <atomic>

#include "BeamPatternModel.hpp"
#include "ComponentBase.hpp"

namespace radar::app {

class Beamformer : public ComponentBase {
public:
    explicit Beamformer(int32_t domain_id)
        : ComponentBase(domain_id, "Radar.Beamformer") {}

    ~Beamformer() override { stop(); }

    void start() override;
    void stop() override;

private:
    void on_beam_command(const types::BeamCommand& command);
    void on_calibration_status(const types::CalibrationStatus& status);
    void publish_loop();

    dds::pub::DataWriter<types::BeamPatternStatus>
        pattern_writer_{dds::core::null};
    dds::sub::DataReader<types::BeamCommand>
        beam_reader_{dds::core::null};
    dds::sub::DataReader<types::CalibrationStatus>
        calibration_reader_{dds::core::null};

    std::atomic<int64_t> beam_id_{-1};
    std::atomic<double> commanded_azimuth_deg_{0.0};
    std::atomic<uint32_t> rma_offline_mask_{0};
};

} // namespace radar::app
