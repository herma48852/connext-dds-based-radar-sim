#pragma once
// HmiUi: the display endpoint of the radar app — a REAL DomainParticipant
// ("Radar.HMI-UI") so that every panel renders data that arrived over the
// DDS bus (no dangling publishers, "DDS all the way to the glass").
//
//   subscribes: Radar/TargetTrack       (track list)
//               Radar/DetectionEvent    (PPI blips)
//               Ship/ShipPosition       (ship panel; key 0 = own-ship INS)
//               Radar/CalibrationStatus (health panel)
//
// Threading rules are unchanged: listener callbacks run on DDS receive
// threads and only convert samples into trivially-copyable view structs
// pushed into the DataBus (lock-free SPSC / mutex-protected stores).
// The render thread itself still never touches DDS.
//
// Deliberately NOT subscribed: Radar/RawReturn (the A-scope trace mirrors
// the 1 kHz stream in-process; a second 1 kHz reader would just burn CPU)
// and Radar/BeamCommand (already consumed by DetectionProcessor; the beam
// timeline mirrors it in-process).

#include <map>
#include <mutex>

#include "ComponentBase.hpp"
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
    void on_track(const types::TargetTrack& t);
    void on_track_dropped(int64_t track_id);
    void on_detection(const types::DetectionEvent& d);
    void on_ship(const types::ShipPosition& s);
    void on_calibration(const types::CalibrationStatus& c);

private:
    void housekeeping_loop(); // publishes track views to the bus, ages out stale

    static constexpr int64_t kTrackStaleMs = 6000; // mirrors tracker coast + margin

    DataBus& bus_;
    dds::sub::DataReader<types::TargetTrack>       track_reader_{dds::core::null};
    dds::sub::DataReader<types::DetectionEvent>    det_reader_{dds::core::null};
    dds::sub::DataReader<types::ShipPosition>      ship_reader_{dds::core::null};
    dds::sub::DataReader<types::CalibrationStatus> cal_reader_{dds::core::null};

    mutable std::mutex tracks_mutex_;
    std::map<int64_t, TrackView> tracks_; // keyed by track_id
};

} // namespace radar::app
