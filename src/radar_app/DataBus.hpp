#pragma once
// ============================================================================
// In-process data bus of the radar app.
//
// Data flow (threading rules strictly enforced):
//   DDS listener threads  --> SpscQueue (lock-free) ----------> render thread
//   DDS worker threads    --> mutex-protected stores --------> render thread
//   CommandHandler thread --> atomic command state ----------> components
//
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

    void update_health(const HealthView& h) {
        std::lock_guard lk(health_mutex_);
        health_ = h;
    }
    HealthView health() const {
        std::lock_guard lk(health_mutex_);
        return health_;
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
    std::atomic<bool>    reset_requested{false};   // consumed by TrackManager
    std::atomic<bool>    self_test_requested{false};

    // Latest beam pointing (DetectionProcessor -> UI sweep display)
    std::atomic<double>  current_beam_az_deg{0.0};
    std::atomic<double>  current_beam_el_deg{2.0};

private:
    mutable std::mutex tracks_mutex_;
    std::vector<TrackView> tracks_;
    mutable std::mutex ship_mutex_;
    ShipView ship_{};
    mutable std::mutex health_mutex_;
    HealthView health_{};
    mutable std::mutex trace_mutex_;
    TraceBuffer trace_front_, trace_back_;
};

} // namespace radar::app
