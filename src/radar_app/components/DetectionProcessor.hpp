#pragma once
// DetectionProcessor: the simulated receiver + signal processor.
//
//   subscribes: Radar/BeamCommand      (listener: track current dwell)
//   subscribes: TargetGen/TargetTruth  (listener: maintain truth cache)
//   subscribes: Radar/BeamPatternStatus (effective array response)
//   publishes : Radar/RawReturn        (1 kHz/face synthesized I/Q)
//   subscribes: Radar/RawReturn        (listener: CFAR detection, loopback)
//   publishes : Radar/DetectionEvent   (threshold crossings)
//
// The RawReturn write->read loopback inside one participant is deliberate:
// it puts four keyed 1 kHz "receiver wires" on the DDS bus where Connext
// Studio can watch them, at ~21.4 MB/s aggregate before DDS overhead.
//
// Detection model (deliberately simple but physically plausible):
//   amplitude  ~ wavelength * sqrt(RCS_linear) * k / R^2
//   range cell : c / (2 * waveform bandwidth)
//   I/Q phase  : 4*pi*range / wavelength, extrapolated at the pulse PRF
//   integration : noncoherent I/Q power average across each 10-pulse dwell
//   detection  : one local-maximum plot per range cell and dwell

#include <array>
#include <chrono>
#include <mutex>
#include <random>
#include <unordered_map>

#include "ComponentBase.hpp"
#include "BeamPatternModel.hpp"
#include "DetectionModel.hpp"
#include "DwellPowerAccumulator.hpp"
#include "RadarFaces.hpp"
#include "../DataBus.hpp"

namespace radar::app {

class DetectionProcessor : public ComponentBase {
public:
    DetectionProcessor(int32_t domain_id, DataBus& bus)
        : ComponentBase(domain_id, "Radar.DetectionProcessor"), bus_(bus) {
        for (const auto& face : faces::kDefinitions) {
            const auto i = static_cast<std::size_t>(face.id);
            dwell_beam_ids_[i].store(-1);
            dwell_az_deg_[i].store(face.boresight_deg);
            dwell_el_deg_[i].store(3.0);
            pattern_revisions_[i].store(0);
            receive_aperture_online_[i].store(true);
        }
    }

    ~DetectionProcessor() override { stop(); }

    void start() override;
    void stop() override;

private:
    struct TruthState {
        double x, y, z;      // ship-relative ENU [m]
        double vx, vy, vz;
        double rcs_dbsm;
        int32_t target_type;
        std::chrono::steady_clock::time_point received_at;
    };

    static constexpr int    kRangeBins      = detection_model::kRangeBins;
    static constexpr double kRangeMinM      = detection_model::kRangeMinM;
    static constexpr double kRangeMaxM      = detection_model::kRangeMaxM;
    static constexpr double kNoiseSigma     = detection_model::kNoiseSigma;
    // Fixed CFAR-like plot threshold applied after ten-pulse power
    // integration. Integration strongly suppresses isolated Rayleigh-noise
    // excursions while retaining the calibrated target-voltage threshold.
    // A fighter (0 dBsm) at 20 km gives ~0.75 before beam loss; nominal
    // center-beam detection range remains approximately 34 km.
    static constexpr double kCfarThreshold  = detection_model::kCfarThreshold;
    void on_beam_command(const types::BeamCommand& cmd);
    void on_beam_pattern(const types::BeamPatternStatus& status);
    void on_truth(const types::TargetTruth& truth);
    void on_raw_return(const types::RawReturn& ret);
    void publish_completed_dwell(
        std::size_t face_index,
        DwellPowerAccumulator& accumulator);
    void return_synthesis_loop();

    DataBus& bus_;

    dds::pub::DataWriter<types::RawReturn>      raw_writer_{dds::core::null};
    dds::pub::DataWriter<types::DetectionEvent> det_writer_{dds::core::null};
    dds::sub::DataReader<types::BeamCommand>    beam_reader_{dds::core::null};
    dds::sub::DataReader<types::BeamPatternStatus>
        pattern_reader_{dds::core::null};
    dds::sub::DataReader<types::RawReturn>      raw_reader_{dds::core::null};
    dds::sub::DataReader<types::TargetTruth>    truth_reader_{dds::core::null};

    // Current dwell (written by BeamCommand listener, read by synth thread)
    std::array<std::atomic<int64_t>, faces::kFaceCount> dwell_beam_ids_{};
    std::array<std::atomic<double>, faces::kFaceCount> dwell_az_deg_{};
    std::array<std::atomic<double>, faces::kFaceCount> dwell_el_deg_{};

    // Effective response (BeamPatternStatus listener -> synth thread).
    mutable std::mutex pattern_mutex_;
    std::array<BeamPattern, faces::kFaceCount> patterns_{};
    std::array<std::atomic<uint64_t>, faces::kFaceCount>
        pattern_revisions_{};
    // Raw receiver noise remains visible when the face is dark, but a zero
    // aperture cannot produce reportable detections.
    std::array<std::atomic<bool>, faces::kFaceCount>
        receive_aperture_online_{};

    // RawReturn listener state. Each keyed face stream is integrated
    // independently and emits at most one local-maximum plot per range cell
    // when beam_id advances to the next 10 ms dwell.
    mutable std::mutex dwell_power_mutex_;
    std::array<DwellPowerAccumulator, faces::kFaceCount>
        dwell_power_accumulators_{};
    std::array<std::vector<float>, faces::kFaceCount>
        integrated_magnitudes_{};

    // Truth cache (TargetTruth listener -> synth thread)
    mutable std::mutex truth_mutex_;
    std::unordered_map<int32_t, TruthState> truth_;

    std::mt19937 rng_{std::random_device{}()};
    std::normal_distribution<float> noise_{0.0f, static_cast<float>(kNoiseSigma)};

    int64_t detection_id_{0};
};

} // namespace radar::app
