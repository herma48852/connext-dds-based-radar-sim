#pragma once
// Beamformer: converts steering intent plus array health into the effective
// outage-aware beam response.
//
//   subscribes: Radar/BeamCommand        (commanded pointing direction)
//   subscribes: Radar/CalibrationStatus  (RMA outage state)
//   publishes : Radar/BeamPatternStatus  (20 Hz/face effective response)

#include <array>
#include <atomic>

#include "BeamPatternModel.hpp"
#include "ComponentBase.hpp"
#include "RadarFaces.hpp"

namespace radar::app {

class Beamformer : public ComponentBase {
public:
    explicit Beamformer(int32_t domain_id)
        : ComponentBase(domain_id, "Radar.Beamformer") {
        for (auto& beam_id : beam_ids_)
            beam_id.store(-1);
        for (auto& azimuth : commanded_azimuth_deg_)
            azimuth.store(0.0);
        for (auto& mask : rma_offline_masks_)
            mask.store(0u);
    }

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

    std::array<std::atomic<int64_t>, faces::kFaceCount> beam_ids_{};
    std::array<std::atomic<double>, faces::kFaceCount>
        commanded_azimuth_deg_{};
    std::array<std::atomic<uint32_t>, faces::kFaceCount>
        rma_offline_masks_{};
};

} // namespace radar::app
