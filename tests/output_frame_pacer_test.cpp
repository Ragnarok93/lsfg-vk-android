#include "output_frame_pacer.hpp"
#include "runtime_policy.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <limits>

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

    // Regression coverage from the 2026-09-03 SM-S731U physical traces: fixed
    // mode must not inherit Adaptive's persisted target or divide source cadence.
    // Adaptive's target is not a fixed-multiplier pacing target. Fixed mode is
    // paced only from an explicit source budget and configured multiplier.
    assert(resolveOutputPacingTarget(false, 60, 30, 2) == 60);
    assert(resolveOutputPacingTarget(false, 60, 30, 3) == 90);
    assert(resolveOutputPacingTarget(false, 120, 30, 3) == 90);

    // Without an explicit source limiter there is no idle budget to spend on
    // sleeps inside vkQueuePresentKHR. Pacing must be disabled so LSFG cannot
    // throttle the real game cadence.
    assert(resolveOutputPacingTarget(false, 60, 0, 2) == 0);
    assert(resolveOutputPacingTarget(true, 120, 0, 2) == 0);

    // Adaptive may space output toward its objective only when a separate source
    // limiter has deliberately created room for those generated presentations.
    // Targets above the configured interpolation capacity are clamped to the
    // sustainable maximum so outputs remain evenly spaced instead of bunching
    // early in each source interval and leaving a visible cadence gap.
    assert(resolveOutputPacingTarget(true, 60, 30, 2) == 60);
    assert(resolveOutputPacingTarget(true, 70, 30, 2) == 60);
    assert(resolveOutputPacingTarget(true, 120, 30, 2) == 60);
    assert(resolveOutputPacingTarget(true, 70, 30, 3) == 70);
    assert(resolveOutputPacingTarget(true, 120, 30, 4) == 120);

    assert(resolveOutputPacingTarget(false, 1,
        std::numeric_limits<uint32_t>::max(), 4)
        == std::numeric_limits<uint32_t>::max());

    // WSI invalidation is single-flight. Rapid setting changes coalesce until
    // the replacement swapchain consumes the newest pending configuration.
    RuntimeWsiRecreateGate gate;
    assert(!gate.inFlight());
    assert(gate.requestIfNeeded(true));
    assert(gate.inFlight());
    assert(!gate.requestIfNeeded(true));
    assert(!gate.requestIfNeeded(false));
    gate.onSwapchainCreation();
    assert(!gate.inFlight());
    assert(gate.requestIfNeeded(true));

    return 0;
}
