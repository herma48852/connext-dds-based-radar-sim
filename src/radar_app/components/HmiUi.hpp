#pragma once
// HmiUi: the display endpoint of the radar app — a REAL DomainParticipant
// ("Radar.HMI-UI") so that every panel renders data that arrived over the
// DDS bus (no dangling publishers, "DDS all the way to the glass").
//
//   subscribes: Radar/TargetTrack       (track list)
//               Radar/DetectionEvent    (PPI blips)
//               Ship/ShipPosition       (source_id = 0 content filter)
//               Radar/CalibrationStatus (health panel)
//               Radar/BeamPatternStatus (B-scope degradation overlay)
//
// Threading rules are unchanged: listener callbacks run on DDS receive
// threads and only convert samples into trivially-copyable view structs
// pushed into the DataBus (lock-free SPSC / mutex-protected stores).
// The render thread itself still never touches DDS.
//
// Deliberately NOT subscribed: Radar/RawReturn (the A-scope trace mirrors
// the four 1 kHz streams in-process; a second 4 kHz aggregate reader would
// just burn CPU)
// and Radar/BeamCommand (already consumed by DetectionProcessor; the beam
// timeline mirrors it in-process).

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>

#include "ComponentBase.hpp"
#include "RadarFaces.hpp"
#include "../DataBus.hpp"

namespace radar::app {

class HmiUi : public ComponentBase {
public:
    HmiUi(int32_t domain_id, DataBus& bus)
        : ComponentBase(domain_id, "Radar.HMI-UI"), bus_(bus) {}

    ~HmiUi() override { stop(); }

    void start() override;
    void stop() override;

    // Listener callbacks (invoked on DDS receive threads).
    void on_track(const types::TargetTrack& t,
                  const dds::core::InstanceHandle& instance_handle);
    void on_track_dropped(
        const dds::core::InstanceHandle& instance_handle);
    void on_detection(const types::DetectionEvent& d);
    void on_ship(const types::ShipPosition& s);
    void on_calibration(const types::CalibrationStatus& c);
    void on_beam_pattern(const types::BeamPatternStatus& p);

private:
    void housekeeping_loop(); // publishes track views to the bus, ages out stale

    // Backstop for a missed DDS disposal. Coasting tracks are still republished
    // at 10 Hz, so this does not shorten TrackerCore's 12-second coast.
    static constexpr auto kTrackStale = std::chrono::seconds(6);

    struct TrackEntry {
        TrackView view;
        dds::core::InstanceHandle instance_handle;
        std::chrono::steady_clock::time_point received_at;
    };

    DataBus& bus_;
    dds::sub::DataReader<types::TargetTrack>       track_reader_{dds::core::null};
    dds::sub::DataReader<types::DetectionEvent>    det_reader_{dds::core::null};
    dds::topic::ContentFilteredTopic<types::ShipPosition>
        ship_topic_{dds::core::null};
    dds::sub::DataReader<types::ShipPosition>      ship_reader_{dds::core::null};
    dds::sub::DataReader<types::CalibrationStatus> cal_reader_{dds::core::null};
    dds::sub::DataReader<types::BeamPatternStatus> pattern_reader_{dds::core::null};
    std::array<std::atomic<uint32_t>, faces::kFaceCount>
        last_pattern_masks_{};

    mutable std::mutex tracks_mutex_;
    std::map<int64_t, TrackEntry> tracks_; // keyed by track_id
};

} // namespace radar::app
