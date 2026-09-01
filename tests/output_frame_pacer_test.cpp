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
    return 0;
}
