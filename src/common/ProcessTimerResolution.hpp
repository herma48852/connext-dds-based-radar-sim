#pragma once

// The radar's shortest periodic loops run at 1 ms (raw pulse synthesis) and
// 10 ms (beam scheduling). Windows otherwise commonly rounds sleep_until
// wake-ups to its coarser default timer quantum, stretching those periods.
// Keep the request scoped to the process lifetime and pair it on shutdown.
#if defined(_WIN32)
#  include <timeapi.h>
#endif

namespace radar {

class ProcessTimerResolution {
public:
    ProcessTimerResolution() noexcept {
#if defined(_WIN32)
        active_ = timeBeginPeriod(kResolutionMs) == TIMERR_NOERROR;
#endif
    }

    ~ProcessTimerResolution() {
#if defined(_WIN32)
        if (active_)
            timeEndPeriod(kResolutionMs);
#endif
    }

    ProcessTimerResolution(const ProcessTimerResolution&) = delete;
    ProcessTimerResolution& operator=(const ProcessTimerResolution&) = delete;

private:
#if defined(_WIN32)
    static constexpr UINT kResolutionMs = 1;
    bool active_ = false;
#endif
};

} // namespace radar
