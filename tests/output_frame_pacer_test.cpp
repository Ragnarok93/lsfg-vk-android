#include "output_frame_pacer.hpp"

#include <cassert>
#include <chrono>

using namespace std::chrono_literals;

int main() {
    OutputFramePacer pacer;
    const auto start = OutputFramePacer::Clock::time_point{};

    pacer.configure(60);
    assert(pacer.delayUntilNext(start) == 0ns);
    const auto secondDelay = pacer.delayUntilNext(start + 1ms);
    assert(secondDelay > 15ms && secondDelay < 17ms);

    // Work performed between presents consumes the cadence budget rather than
    // adding another full period of latency.
    const auto thirdDelay = pacer.delayUntilNext(start + 20ms);
    assert(thirdDelay > 12ms && thirdDelay < 14ms);

    // A long stall discards stale deadlines and resumes without a catch-up burst.
    assert(pacer.delayUntilNext(start + 1s) == 0ns);

    pacer.configure(0);
    assert(pacer.delayUntilNext(start + 1s) == 0ns);

    // Fractional nanoseconds are accumulated instead of truncated. Ninety 90 Hz
    // periods should land at ~1 second, not drift early by repeated integer loss.
    pacer.configure(90);
    assert(pacer.delayUntilNext(start) == 0ns);
    std::chrono::nanoseconds lastDelay{};
    for (int i = 0; i < 90; ++i)
        lastDelay = pacer.delayUntilNext(start);
    assert(lastDelay >= 999999999ns && lastDelay <= 1000000001ns);

    return 0;
}
