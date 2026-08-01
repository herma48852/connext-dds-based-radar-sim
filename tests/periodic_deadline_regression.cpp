#include "PeriodicDeadline.hpp"

#include <chrono>
#include <cstdio>

namespace {
int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}
} // namespace

int main() {
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;

    const Clock::time_point start{10s};
    const auto nominal =
        radar::advance_periodic_deadline(start, 10ms, start + 2ms);
    check(nominal == start + 10ms,
          "on-time loop preserves its fixed-rate deadline");

    // A coarse timer may wake just after a deadline. Keep the phase and let
    // the loop make up this small delay instead of permanently lowering its
    // average rate by scheduling a fresh full period from the late wake-up.
    const auto jittered = radar::advance_periodic_deadline(
        start + 20ms, 10ms, start + 31ms);
    check(jittered == start + 30ms,
          "ordinary wake-up jitter preserves the original deadline phase");

    const auto recovered = radar::advance_periodic_deadline(
        jittered, 10ms, start + 31ms);
    check(recovered == start + 40ms,
          "one bounded catch-up restores the normal cadence");

    // Simulate an overnight machine sleep. The old next += period behavior
    // left the deadline eight hours behind and replayed 2.88 million 10 ms
    // ticks at maximum speed after wake.
    const auto wake = start + 8h;
    const auto resumed =
        radar::advance_periodic_deadline(nominal, 10ms, wake);
    check(resumed == wake + 10ms,
          "eight-hour pause resynchronizes to one future tick");

    const auto following =
        radar::advance_periodic_deadline(resumed, 10ms, resumed);
    check(following == resumed + 10ms,
          "resynchronized loop resumes its normal cadence");

    if (failures != 0) {
        std::fprintf(stderr,
                     "periodic_deadline_regression: %d failure(s)\n",
                     failures);
        return 1;
    }
    std::printf("periodic_deadline_regression: PASS\n");
    return 0;
}
