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
// Radar.HMI-UI participant's DDS readers. Operational own-ship state also
// travels over DDS directly to DetectionProcessor and TrackManager. Only
// render-only traces and UI control state remain in-process here.
// The render thread never blocks on DDS; DDS threads never touch ImGui/GL.
// ============================================================================

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "RadarFaces.hpp"
#include "SpscQueue.hpp"

namespace radar::app {

// Trivially-copyable view types crossing the lock-free boundary.
struct BlipView {
    int32_t face_id;
    double  range_m;
    double  azimuth_deg;
    double  elevation_deg;
    double  amplitude;
    double  snr_db;
    int64_t sim_millis;
};

struct BeamView {
    int32_t face_id;
    int64_t beam_id;
    double  azimuth_deg;
    double  elevation_deg;
    int32_t mode;         // radar::types::BeamMode
    int32_t priority;
    int64_t sim_millis;
};

struct BeamPatternView {
    int32_t face_id = 0;
    int64_t beam_id = 0;
    uint32_t rma_mask = 0;
    double commanded_azimuth_deg = 0.0;
    double boresight_error_deg = 0.0;
    double gain_loss_db = 0.0;
    double beamwidth_3db_deg = 3.2;
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
    int32_t face_id;
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
    int32_t            face_id = 0;
    std::vector<float> drift_db;   // 1024 values, 32x32 row-major
    uint32_t           rma_mask = 0;
    int64_t            sim_millis = 0;
};

// Latest A-scope trace (double-buffered; writer swaps under lock).
struct TraceBuffer {
    int32_t face_id        = 0;
    std::vector<float> magnitude;  // linear magnitude per range bin
    double  azimuth_deg = 0.0;
    double  elevation_deg = 0.0;
    double  range_max_m  = 100000.0;
    int64_t beam_id      = 0;
    uint32_t sequence    = 0;
};

class DataBus {
public:
    DataBus() {
        for (const auto& face : faces::kDefinitions) {
            const auto i = face_index(face.id);
            radar_mode[i].store(0);
            sector_center_deg[i].store(face.boresight_deg);
            sector_width_deg[i].store(30.0);
            degrade_array[i].store(false);
            rma_offline_mask[i].store(0u);
            current_beam_az_deg[i].store(face.boresight_deg);
            current_beam_el_deg[i].store(3.0);
            health_[i].face_id = face.id;
            health_[i].total_elements = 1024;
            array_grid_[i].face_id = face.id;
            beam_pattern_[i].face_id = face.id;
            trace_front_[i].face_id = face.id;
            trace_back_[i].face_id = face.id;
        }
    }

    // --- lock-free paths (DDS listener -> render) ---
    SpscQueue<BlipView> detection_blips{4096};
    SpscQueue<BeamView> beam_commands{4096};

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

    // Ship PANEL data: fed by HmiUi's Ship/ShipPosition reader (key 0).
    void update_ship_display(const ShipView& s) {
        std::lock_guard lk(ship_display_mutex_);
        ship_display_ = s;
    }
    ShipView ship_display() const {
        std::lock_guard lk(ship_display_mutex_);
        return ship_display_;
    }

    void update_health(const HealthView& h) {
        if (!faces::valid(h.face_id))
            return;
        std::lock_guard lk(health_mutex_);
        health_[face_index(h.face_id)] = h;
    }
    HealthView health(int32_t face_id) const {
        std::lock_guard lk(health_mutex_);
        return health_[face_index(face_id)];
    }

    void update_array_grid(int32_t face_id, const std::vector<float>& drift,
                           uint32_t mask, int64_t ms) {
        if (!faces::valid(face_id))
            return;
        std::lock_guard lk(array_grid_mutex_);
        auto& grid = array_grid_[face_index(face_id)];
        grid.face_id    = face_id;
        grid.drift_db   = drift;
        grid.rma_mask   = mask;
        grid.sim_millis = ms;
    }
    ArrayGridView array_grid(int32_t face_id) const {
        std::lock_guard lk(array_grid_mutex_);
        return array_grid_[face_index(face_id)];
    }

    void update_beam_pattern(const BeamPatternView& pattern) {
        if (!faces::valid(pattern.face_id))
            return;
        std::lock_guard lk(beam_pattern_mutex_);
        beam_pattern_[face_index(pattern.face_id)] = pattern;
    }
    BeamPatternView beam_pattern(int32_t face_id) const {
        std::lock_guard lk(beam_pattern_mutex_);
        return beam_pattern_[face_index(face_id)];
    }

    void update_trace(const TraceBuffer& t) {
        if (!faces::valid(t.face_id))
            return;
        std::lock_guard lk(trace_mutex_);
        const auto i = face_index(t.face_id);
        if (trace_back_[i].magnitude.size() != t.magnitude.size())
            trace_back_[i].magnitude.resize(t.magnitude.size());
        trace_back_[i] = t;
        trace_back_[i].sequence++;
        std::swap(trace_back_[i], trace_front_[i]);
    }
    TraceBuffer trace(int32_t face_id) const {
        std::lock_guard lk(trace_mutex_);
        return trace_front_[face_index(face_id)];
    }

    std::array<double, faces::kFaceCount> beam_azimuths() const {
        std::array<double, faces::kFaceCount> result{};
        for (std::size_t i = 0; i < result.size(); ++i)
            result[i] = current_beam_az_deg[i].load();
        return result;
    }

    // --- command state (CommandHandler -> components), all atomic ---
    std::array<std::atomic<int32_t>, faces::kFaceCount>
        radar_mode{}; // 0 = search, 1 = sector scan
    std::array<std::atomic<double>, faces::kFaceCount> sector_center_deg{};
    std::array<std::atomic<double>, faces::kFaceCount> sector_width_deg{};
    std::array<std::atomic<bool>, faces::kFaceCount>
        degrade_array{}; // demo: CalibrationStatus faults
    // RMA-offline state (bit i = RMA i offline, 16 RMAs x 64 elements).
    // Written only by CommandHandler (CMD_RMA_OFFLINE/ONLINE); read by
    // CalibrationMonitor, which publishes it for Beamformer over DDS.
    std::array<std::atomic<uint32_t>, faces::kFaceCount>
        rma_offline_mask{};
    std::atomic<bool>    reset_requested{false};   // consumed by TrackManager
    std::atomic<bool>    self_test_requested{false};
    // Crash-investigation toggle (--no-dispose): when false, TrackManager
    // skips every dispose_instance call. Dropped tracks then linger as DDS
    // instances (the UI still ages them out) — acceptable for a short
    // experiment that isolates the dispose path as a crash suspect.
    std::atomic<bool>    dispose_enabled{true};

    // Latest beam pointing (BeamScheduler -> UI sweep display)
    std::array<std::atomic<double>, faces::kFaceCount> current_beam_az_deg{};
    std::array<std::atomic<double>, faces::kFaceCount> current_beam_el_deg{};

private:
    static constexpr std::size_t face_index(int32_t face_id) noexcept {
        return static_cast<std::size_t>(
            faces::valid(face_id) ? face_id : faces::kForwardStarboard);
    }

    mutable std::mutex tracks_mutex_;
    std::vector<TrackView> tracks_;
    mutable std::mutex ship_display_mutex_;
    ShipView ship_display_{};
    mutable std::mutex health_mutex_;
    std::array<HealthView, faces::kFaceCount> health_{};
    mutable std::mutex array_grid_mutex_;
    std::array<ArrayGridView, faces::kFaceCount> array_grid_{};
    mutable std::mutex beam_pattern_mutex_;
    std::array<BeamPatternView, faces::kFaceCount> beam_pattern_{};
    mutable std::mutex trace_mutex_;
    std::array<TraceBuffer, faces::kFaceCount> trace_front_{};
    std::array<TraceBuffer, faces::kFaceCount> trace_back_{};
};

} // namespace radar::app
