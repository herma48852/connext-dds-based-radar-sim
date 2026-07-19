#include "DetectionProcessor.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>

#include "SimClock.hpp"

namespace radar::app {

namespace {
constexpr double kDeg2Rad = 3.14159265358979323846 / 180.0;

double wrap180(double a) {
    while (a > 180.0)  a -= 360.0;
    while (a < -180.0) a += 360.0;
    return a;
}

// Generic listener adapter: forwards whole loaned batches to a member
// function. Runs on the DDS receive thread -> keep the handler light.
template <typename T, typename Owner, void (Owner::*Method)(const T&)>
class ForwardingListener : public dds::sub::NoOpDataReaderListener<T> {
public:
    explicit ForwardingListener(Owner* owner) : owner_(owner) {}
    void on_data_available(dds::sub::DataReader<T>& reader) override {
        auto samples = reader.take();
        for (const auto& s : samples)
            if (s.info().valid())
                (owner_->*Method)(s.data());
    }
private:
    Owner* owner_;
};
} // namespace

void DetectionProcessor::start() {
    auto beam_topic  = radds::make_topic<types::BeamCommand>(participant_, dds_names::TOPIC_BEAM_COMMAND);
    auto raw_topic   = radds::make_topic<types::RawReturn>(participant_, dds_names::TOPIC_RAW_RETURN);
    auto det_topic   = radds::make_topic<types::DetectionEvent>(participant_, dds_names::TOPIC_DETECTION_EVENT);
    auto truth_topic = radds::make_topic<types::TargetTruth>(participant_, dds_names::TOPIC_TARGET_TRUTH);

    raw_writer_   = radds::make_writer<types::RawReturn>(publisher_, raw_topic, dds_names::PROFILE_RAW_RETURN);
    det_writer_   = radds::make_writer<types::DetectionEvent>(publisher_, det_topic, dds_names::PROFILE_DETECTION_EVENT);
    beam_reader_  = radds::make_reader<types::BeamCommand>(subscriber_, beam_topic, dds_names::PROFILE_BEAM_COMMAND);
    raw_reader_   = radds::make_reader<types::RawReturn>(subscriber_, raw_topic, dds_names::PROFILE_RAW_RETURN);
    truth_reader_ = radds::make_reader<types::TargetTruth>(subscriber_, truth_topic, dds_names::PROFILE_TARGET_TRUTH);

    // Listeners for the high-rate paths (BeamCommand, RawReturn, TargetTruth).
    beam_reader_.set_listener(
        std::make_shared<ForwardingListener<types::BeamCommand, DetectionProcessor,
                                            &DetectionProcessor::on_beam_command>>(this),
        dds::core::status::StatusMask::data_available());
    raw_reader_.set_listener(
        std::make_shared<ForwardingListener<types::RawReturn, DetectionProcessor,
                                            &DetectionProcessor::on_raw_return>>(this),
        dds::core::status::StatusMask::data_available());
    truth_reader_.set_listener(
        std::make_shared<ForwardingListener<types::TargetTruth, DetectionProcessor,
                                            &DetectionProcessor::on_truth>>(this),
        dds::core::status::StatusMask::data_available());

    spawn([this] { return_synthesis_loop(); });
}

void DetectionProcessor::on_beam_command(const types::BeamCommand& cmd) {
    dwell_beam_id_.store(cmd.beam_id);
    dwell_az_deg_.store(cmd.azimuth_deg);
    dwell_el_deg_.store(cmd.elevation_deg);
}

void DetectionProcessor::on_truth(const types::TargetTruth& t) {
    std::lock_guard lk(truth_mutex_);
    auto& s = truth_[t.target_id];
    s.x  = t.position.x_east_m;  s.y  = t.position.y_north_m;  s.z  = t.position.z_up_m;
    s.vx = t.velocity.x_east_m;  s.vy = t.velocity.y_north_m;  s.vz = t.velocity.z_up_m;
    s.rcs_dbsm    = t.rcs_dbsm;
    s.target_type = static_cast<int32_t>(t.target_type);
    s.last_sim_ms = t.timestamp.sim_millis;
}

// --- Receiver simulation: 1 kHz RawReturn synthesis for the current dwell --
void DetectionProcessor::return_synthesis_loop() {
    using namespace std::chrono;
    auto next = steady_clock::now();

    types::RawReturn sample;
    sample.range_bin_count = kRangeBins;
    sample.iq_samples.resize(2 * kRangeBins);

    while (!stop_.load()) {
        next += milliseconds(1); // 1 kHz simulated PRF

        const int64_t beam_id = dwell_beam_id_.load();
        if (beam_id < 0) { // no dwell scheduled yet
            std::this_thread::sleep_until(next);
            continue;
        }
        const double az_deg = dwell_az_deg_.load();
        const double el_deg = dwell_el_deg_.load();

        // Noise floor
        for (int i = 0; i < 2 * kRangeBins; ++i)
            sample.iq_samples[i] = noise_(rng_);

        // Implant targets inside the beam (snapshot truth under lock)
        {
            std::lock_guard lk(truth_mutex_);
            const double heading = bus_.ship().heading_deg;
            for (const auto& [id, t] : truth_) {
                const double range_xy = std::hypot(t.x, t.y);
                const double range    = std::sqrt(t.x*t.x + t.y*t.y + t.z*t.z);
                if (range < 100.0 || range > kRangeMaxM) continue;

                // world ENU azimuth -> ship-relative azimuth
                const double az_world = std::atan2(t.x, t.y) / kDeg2Rad;
                const double az_ship  = wrap180(az_world - heading);
                if (std::fabs(wrap180(az_ship - az_deg)) > kBeamwidthDeg * 0.5)
                    continue; // outside this dwell's beam

                const double el_t = std::atan2(t.z, range_xy) / kDeg2Rad;
                if (std::fabs(el_t - el_deg) > 3.0)
                    continue; // outside elevation beam

                const double rcs_lin = std::pow(10.0, t.rcs_dbsm / 10.0);
                const double amp = kSignalScale * std::sqrt(rcs_lin) / (range * range);

                const double bin_f = range / kRangeMaxM * kRangeBins;
                const int b0 = static_cast<int>(bin_f);
                // spread the return over ~3 bins (matched-filter response)
                for (int db = -1; db <= 1; ++db) {
                    const int b = b0 + db;
                    if (b < 0 || b >= kRangeBins) continue;
                    const double w = (db == 0) ? 1.0 : 0.4;
                    sample.iq_samples[2*b]   += static_cast<float>(amp * w);
                    sample.iq_samples[2*b+1] += static_cast<float>(amp * w * 0.3);
                }
            }
        }

        sample.beam_id        = beam_id;
        sample.timestamp      = SimClock::stamp();
        sample.azimuth_deg    = az_deg;
        sample.elevation_deg  = el_deg;
        raw_writer_.write(sample);

        std::this_thread::sleep_until(next);
    }
}

// --- Signal processor: CFAR threshold crossings -> DetectionEvent ----------
void DetectionProcessor::on_raw_return(const types::RawReturn& ret) {
    const int n = std::min<int>(ret.range_bin_count, kRangeBins);
    if (n < 3) return;

    // magnitude per bin (also feeds the A-scope trace)
    static thread_local std::vector<float> mag;
    mag.resize(n);
    for (int i = 0; i < n; ++i) {
        const float I = ret.iq_samples[2*i];
        const float Q = ret.iq_samples[2*i+1];
        mag[i] = std::sqrt(I*I + Q*Q);
    }

    TraceBuffer tb;
    tb.magnitude     = mag;
    tb.azimuth_deg   = ret.azimuth_deg;
    tb.elevation_deg = ret.elevation_deg;
    tb.range_max_m   = kRangeMaxM;
    tb.beam_id       = ret.beam_id;
    bus_.update_trace(tb);

    const auto ship = bus_.ship();
    types::GeoPosition geo;
    geo.latitude_deg  = ship.latitude_deg;
    geo.longitude_deg = ship.longitude_deg;
    geo.altitude_m    = ship.altitude_m;

    // Peak picking above the CFAR threshold
    for (int i = 1; i < n - 1; ++i) {
        if (mag[i] > kCfarThreshold && mag[i] >= mag[i-1] && mag[i] > mag[i+1]) {
            const double range_m = (static_cast<double>(i) / n) * kRangeMaxM;
            const double snr_db  = 20.0 * std::log10(mag[i] / kNoiseSigma);

            types::DetectionEvent det;
            det.sensor_id     = 0; // constant key: one DDS instance
            det.detection_id  = detection_id_++;
            det.timestamp     = SimClock::stamp();
            det.ship_position = geo;
            det.range_m       = range_m;
            det.azimuth_deg   = ret.azimuth_deg;
            det.elevation_deg = ret.elevation_deg;
            det.amplitude     = mag[i];
            det.snr_db        = snr_db;
            det_writer_.write(det);
            // PPI blips reach the UI via HmiUi's DetectionEvent subscription.
        }
    }
}

} // namespace radar::app
