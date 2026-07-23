#pragma once
// ============================================================================
// Portable azimuth beam-pattern model for the 32x32 array face.
//
// The model consumes the 16-bit RMA-offline mask and produces a normalized
// one-dimensional azimuth cut plus scalar health metrics.  It has no DDS or
// UI dependencies so the beamformer, receiver, B-scope overlay, and regression
// test share one deterministic source of truth.
// ============================================================================

#include <array>
#include <cstdint>

namespace radar::app {

inline constexpr int kBeamPatternSampleCount = 181;
inline constexpr double kBeamPatternStartDeg = -45.0;
inline constexpr double kBeamPatternStepDeg = 0.5;

struct BeamPattern {
    uint32_t rma_offline_mask = 0;
    int active_elements = 1024;
    double active_fraction = 1.0;
    double gain_loss_db = 0.0;
    double boresight_error_deg = 0.0;
    double beamwidth_3db_deg = 2.0;
    double peak_sidelobe_level_db = -13.0;
    double left_sidelobe_offset_deg = 0.0;
    double right_sidelobe_offset_deg = 0.0;
    std::array<float, kBeamPatternSampleCount> azimuth_pattern_db{};

    // Pattern amplitude relative to the active-aperture peak. Values outside
    // the published +/-45-degree cut are treated as zero.
    double relative_amplitude(double offset_deg) const;
};

class BeamPatternModel {
public:
    static BeamPattern calculate(uint32_t rma_offline_mask);
};

} // namespace radar::app
