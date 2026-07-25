#pragma once
// ============================================================================
// Simulation clock: dual time base (wall clock + sim time) stamped onto
// every DDS sample. Thread-safe, started once at process start.
//
// NOTE: rtiddsgen's modern C++11 mapping generates IDL structs as plain
// structs with PUBLIC DATA MEMBERS (not accessor methods):
//     t.epoch_millis = value;   // not t.epoch_millis(value)
// ============================================================================

#include <chrono>
#include <cstdint>
#include "radar_types.hpp" // generated from idl/radar_types.idl

namespace radar {

class SimClock {
public:
    static void start() {
        (void)start_time();
    }

    static int64_t sim_millis() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   clock::now() - start_time()).count();
    }

    static int64_t epoch_millis() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // Fill the IDL TimeStamp used by every topic.
    static ::radar::types::TimeStamp stamp() {
        ::radar::types::TimeStamp t;
        t.epoch_millis = epoch_millis();
        t.sim_millis   = static_cast<int32_t>(sim_millis());
        return t;
    }

private:
    using clock = std::chrono::steady_clock;
    static const clock::time_point& start_time() {
        static const clock::time_point started = clock::now();
        return started;
    }
};

} // namespace radar
