#pragma once
// DetectionProcessor: the simulated receiver + signal processor.
//
//   subscribes: Radar/BeamCommand      (listener: track current dwell)
//   subscribes: TargetGen/TargetTruth  (listener: maintain truth cache)
//   publishes : Radar/RawReturn        (1 kHz synthesized I/Q per dwell)
//   subscribes: Radar/RawReturn        (listener: CFAR detection, loopback)
//   publishes : Radar/DetectionEvent   (threshold crossings)
//
// The RawReturn write->read loopback inside one participant is deliberate:
// it puts the 1 kHz "receiver wire" on the DDS bus where Connext Studio can
// watch it, at ~2 MB/s over loopback/shared memory.
//
// Detection model (deliberately simple but physically plausible):
//   amplitude  ~ sqrt(RCS_linear) * k / R^2   (radar equation, amplitude)
//   detection  : magnitude > CFAR threshold above the noise floor

#include <array>
#include <mutex>
#include <random>
#include <unordered_map>

#include "ComponentBase.hpp"
#include "../DataBus.hpp"

namespace radar::app {

class DetectionProcessor : public ComponentBase {
public:
    DetectionProcessor(int32_t domain_id, DataBus& bus)
        : ComponentBase(domain_id, "Radar.DetectionProcessor"), bus_(bus) {}

    void start() override;

private:
    struct TruthState {
        double x, y, z;      // ship-relative ENU [m]
        double vx, vy, vz;
        double rcs_dbsm;
        int32_t target_type;
        int64_t last_sim_ms;
    };

    static constexpr int    kRangeBins      = 512;
    static constexpr double kRangeMaxM      = 100000.0;  // 100 km instrumented
    static constexpr double kBeamwidthDeg   = 2.0;
    static constexpr double kNoiseSigma     = 0.05;
    // CFAR: for Rayleigh noise, Pfa = exp(-T^2 / (2*sigma^2)).
    // T = 0.26 -> Pfa ~ 1.4e-6 per bin (a few false alarms per second,
    // not tens of thousands). Amplitudes: fighter (0 dBsm) at 20 km
    // gives ~0.5 -> ~20 dB SNR, detection range ~28 km.
    static constexpr double kCfarThreshold  = 0.26;
    static constexpr double kSignalScale    = 2.0e8;     // amplitude = scale*sqrt(rcs)/R^2

    void on_beam_command(const types::BeamCommand& cmd);
    void on_truth(const types::TargetTruth& truth);
    void on_raw_return(const types::RawReturn& ret);
    void return_synthesis_loop();

    DataBus& bus_;

    dds::pub::DataWriter<types::RawReturn>      raw_writer_{dds::core::null};
    dds::pub::DataWriter<types::DetectionEvent> det_writer_{dds::core::null};
    dds::sub::DataReader<types::BeamCommand>    beam_reader_{dds::core::null};
    dds::sub::DataReader<types::RawReturn>      raw_reader_{dds::core::null};
    dds::sub::DataReader<types::TargetTruth>    truth_reader_{dds::core::null};

    // Current dwell (written by BeamCommand listener, read by synth thread)
    std::atomic<int64_t> dwell_beam_id_{-1};
    std::atomic<double>  dwell_az_deg_{0.0};
    std::atomic<double>  dwell_el_deg_{2.0};

    // Truth cache (TargetTruth listener -> synth thread)
    mutable std::mutex truth_mutex_;
    std::unordered_map<int32_t, TruthState> truth_;

    std::mt19937 rng_{std::random_device{}()};
    std::normal_distribution<float> noise_{0.0f, static_cast<float>(kNoiseSigma)};

    int64_t detection_id_{0};
};

} // namespace radar::app
