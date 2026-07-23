#pragma once
// ============================================================================
// In-process data bus of the radar app.
//
// Data flow (threading rules strictly enforced):
//   HMI-UI DDS listeners  --> SpscQueue (lock-free) ----------> render thread
//   HMI-UI DDS listeners  --> mutex-protected stores --------> render thread
//   component threads     --> mutex-protected stores --------> render thread
//   CommandHandler thread --> atomic command state ----------> components
//
// Display data (tracks, blips, ship panel, health panel) arrives via the
// Radar.HMI-UI participant's DDS readers. Radar-function data shared between
// components (own-ship state, A-scope trace, beam pointing) stays in-process.
// The render thread never blocks on DDS; DDS threads never touch ImGui/GL.
// ============================================================================

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "SpscQueue.hpp"

namespace radar::app {

// Trivially-copyable view types crossing the lock-free boundary.
struct BlipView {
    double  range_m;
    double  azimuth_deg;
    double  elevation_deg;
    double  amplitude;
    double  snr_db;
    int64_t sim_millis;
};

struct BeamView {
    int64_t beam_id;
    double  azimuth_deg;
    double  elevation_deg;
    int32_t mode;         // radar::types::BeamMode
    int32_t priority;
    int64_t sim_millis;
};

struct BeamPatternView {
    int64_t beam_id = 0;
    uint32_t rma_mask = 0;
    double commanded_azimuth_deg = 0.0;
    double boresight_error_deg = 0.0;
    double gain_loss_db = 0.0;
    double beamwidth_3db_deg = 2.0;
    double peak_sidelobe_level_db = -80.0;
    double left_sidelobe_offset_deg = 0.0;
    double right_sidelobe_offset_deg = 0.0;
    double pattern_start_offset_deg = -45.0;
    double pattern_step_deg = 0.5;
    std::vector<float> azimuth_pattern_db;
    int64_t sim_millis = 0;
};

struct TrackView {
    int64_t track_id;
    double  x_m, y_m, z_m;        // ship-relative ENU
    double  vx_mps, vy_mps, vz_mps;
    int32_t classification;
    int32_t quality;
    int64_t sim_millis;
};

struct ShipView {
    double latitude_deg, longitude_deg, altitude_m;
    double heading_deg, course_deg, speed_mps;
    double pitch_deg, roll_deg;
    int64_t sim_millis;
};

struct HealthView {
    int32_t overall_status;      // radar::types::ArrayHealth
    int32_t failed_element_count;
    int32_t total_elements;
    double  temperature_c;
    double  mean_abs_drift_db;
    int64_t sim_millis;
    uint32_t rma_mask = 0;       // bit i = RMA i offline (16 RMAs)
};

// Latest array-face state for the ARRAY FACE pane (HMI-UI's
// CalibrationStatus reader -> render thread). Full 1024-value drift
// vector at 1 Hz; copied per frame, negligible.
struct ArrayGridView {
    std::vector<float> drift_db;   // 1024 values, 32x32 row-major
    uint32_t           rma_mask = 0;
    int64_t            sim_millis = 0;
};

// Latest A-scope trace (double-buffered; writer swaps under lock).
struct TraceBuffer {
    std::vector<float> magnitude;  // linear magnitude per range bin
    double  azimuth_deg = 0.0;
    double  elevation_deg = 0.0;
    double  range_max_m  = 100000.0;
    int64_t beam_id      = 0;
    uint32_t sequence    = 0;
};

class DataBus {
public:
    // --- lock-free paths (DDS listener -> render) ---
    SpscQueue<BlipView> detection_blips{4096};
    SpscQueue<BeamView> beam_commands{1024};

    // --- mutex-protected stores ---
    void update_tracks(const std::vector<TrackView>& t) {
        std::lock_guard lk(tracks_mutex_);
        tracks_ = t;
    }
    std::vector<TrackView> tracks() const {
        std::lock_guard lk(tracks_mutex_);
        return tracks_;
    }
    void clear_tracks() {
        std::lock_guard lk(tracks_mutex_);
        tracks_.clear();
    }

    void update_ship(const ShipView& s) {
        std::lock_guard lk(ship_mutex_);
        ship_ = s;
    }
    ShipView ship() const {
        std::lock_guard lk(ship_mutex_);
        return ship_;
    }

    // Ship PANEL data: fed by HmiUi's Ship/ShipPosition reader (key 0).
    // Kept separate from ship() above, which is the radar-function path
    // (ShipSimulator -> DetectionProcessor/TrackManager) so the HMI can
    // never perturb component behaviour.
    void update_ship_display(const ShipView& s) {
        std::lock_guard lk(ship_display_mutex_);
        ship_display_ = s;
    }
    ShipView ship_display() const {
        std::lock_guard lk(ship_display_mutex_);
        return ship_display_;
    }

    void update_health(const HealthView& h) {
        std::lock_guard lk(health_mutex_);
        health_ = h;
    }
    HealthView health() const {
        std::lock_guard lk(health_mutex_);
        return health_;
    }

    void update_array_grid(const std::vector<float>& drift, uint32_t mask,
                           int64_t ms) {
        std::lock_guard lk(array_grid_mutex_);
        array_grid_.drift_db   = drift;
        array_grid_.rma_mask   = mask;
        array_grid_.sim_millis = ms;
    }
    ArrayGridView array_grid() const {
        std::lock_guard lk(array_grid_mutex_);
        return array_grid_;
    }

    void update_beam_pattern(const BeamPatternView& pattern) {
        std::lock_guard lk(beam_pattern_mutex_);
        beam_pattern_ = pattern;
    }
    BeamPatternView beam_pattern() const {
        std::lock_guard lk(beam_pattern_mutex_);
        return beam_pattern_;
    }

    void update_trace(const TraceBuffer& t) {
        std::lock_guard lk(trace_mutex_);
        if (trace_back_.magnitude.size() != t.magnitude.size())
            trace_back_.magnitude.resize(t.magnitude.size());
        trace_back_ = t;
        trace_back_.sequence++;
        std::swap(trace_back_, trace_front_);
    }
    TraceBuffer trace() const {
        std::lock_guard lk(trace_mutex_);
        return trace_front_;
    }

    // --- command state (CommandHandler -> components), all atomic ---
    std::atomic<int32_t> radar_mode{0};            // 0 = search, 1 = sector scan
    std::atomic<double>  sector_center_deg{90.0};
    std::atomic<double>  sector_width_deg{60.0};
    std::atomic<bool>    degrade_array{false};     // demo: CalibrationStatus faults
    // RMA-offline state (bit i = RMA i offline, 16 RMAs x 64 elements).
    // Written only by CommandHandler (CMD_RMA_OFFLINE/ONLINE); read by
    // CalibrationMonitor (drift/status) and DetectionProcessor (gain/beam).
    std::atomic<uint32_t> rma_offline_mask{0};
    std::atomic<bool>    reset_requested{false};   // consumed by TrackManager
    std::atomic<bool>    self_test_requested{false};
    // Crash-investigation toggle (--no-dispose): when false, TrackManager
    // skips every dispose_instance call. Dropped tracks then linger as DDS
    // instances (the UI still ages them out) — acceptable for a short
    // experiment that isolates the dispose path as a crash suspect.
    std::atomic<bool>    dispose_enabled{true};

    // Latest beam pointing (DetectionProcessor -> UI sweep display)
    std::atomic<double>  current_beam_az_deg{0.0};
    std::atomic<double>  current_beam_el_deg{2.0};

private:
    mutable std::mutex tracks_mutex_;
    std::vector<TrackView> tracks_;
    mutable std::mutex ship_mutex_;
    ShipView ship_{};
    mutable std::mutex ship_display_mutex_;
    ShipView ship_display_{};
    mutable std::mutex health_mutex_;
    HealthView health_{};
    mutable std::mutex array_grid_mutex_;
    ArrayGridView array_grid_;
    mutable std::mutex beam_pattern_mutex_;
    BeamPatternView beam_pattern_;
    mutable std::mutex trace_mutex_;
    TraceBuffer trace_front_, trace_back_;
};

} // namespace radar::app
