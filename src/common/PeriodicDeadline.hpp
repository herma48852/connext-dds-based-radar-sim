#pragma once

#include <chrono>

namespace radar {

// Advance a fixed-rate loop. Preserve the original phase through a small
// amount of ordinary wake-up jitter so the average rate does not drift, but
// resynchronize after process suspension, machine sleep, debugger pauses, or
// severe scheduler stalls instead of replaying an unbounded backlog.
template <typename Rep, typename Period>
std::chrono::steady_clock::time_point advance_periodic_deadline(
        std::chrono::steady_clock::time_point previous,
        std::chrono::duration<Rep, Period> period,
        std::chrono::steady_clock::time_point now =
            std::chrono::steady_clock::now()) {
    using Clock = std::chrono::steady_clock;
    const auto tick =
        std::chrono::duration_cast<Clock::duration>(period);
    const auto scheduled = previous + tick;
    constexpr int kMaxCatchUpPeriods = 4;
    return now - scheduled >= tick * kMaxCatchUpPeriods
        ? now + tick
        : scheduled;
}

} // namespace radar
