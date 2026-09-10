#include "adaptive_scheduler.hpp"

#include <cassert>
#include <chrono>
#include <cstddef>

using namespace std::chrono_literals;

int main() {
    {
        AdaptiveFrameScheduler scheduler(60, 3);
        assert(scheduler.plan(33333333ns) == 1);
        assert(scheduler.plan(33333333ns) == 1);
        assert(scheduler.plan(33333333ns) == 1);
    }

    {
        AdaptiveFrameScheduler scheduler(60, 3);
        std::size_t generated = 0;
        for (int frame = 0; frame < 50; ++frame)
            generated += scheduler.plan(20ms);
        assert(generated >= 9 && generated <= 11);
    }

    {
        AdaptiveFrameScheduler scheduler(60, 3);
        for (int frame = 0; frame < 20; ++frame)
            assert(scheduler.plan(10ms) == 0);
    }

    {
        // Adaptive generation must ramp GPU cost instead of jumping directly
        // to the maximum interpolation load on the first slow source frame.
        AdaptiveFrameScheduler scheduler(120, 3);
        assert(scheduler.plan(40ms) == 1);
    }

    {
        // A sustained unmet target may raise generation cost one level after
        // the observation interval, never directly from 1 to the user maximum.
        AdaptiveFrameScheduler scheduler(120, 3);
        bool sawRaise = false;
        for (int frame = 0; frame < 10; ++frame) {
            scheduler.plan(40ms);
            sawRaise = sawRaise || scheduler.telemetry().costRaised;
        }
        assert(sawRaise);
        assert(scheduler.telemetry().costLimit >= 2);
    }

    {
        // If source FPS collapses shortly after a cost raise, attribute the
        // correlated drop to framegen and return to the lower cost ceiling.
        AdaptiveFrameScheduler scheduler(120, 3);
        bool sawRaise = false;
        for (int frame = 0; frame < 7; ++frame) {
            scheduler.plan(40ms);
            sawRaise = sawRaise || scheduler.telemetry().costRaised;
        }
        bool sawBackoff = false;
        for (int frame = 0; frame < 3; ++frame) {
            scheduler.plan(60ms);
            sawBackoff = sawBackoff || scheduler.telemetry().costBackedOff;
        }
        assert(sawRaise);
        assert(sawBackoff);
        assert(scheduler.telemetry().costLimit == 1);
    }

    {
        // A natural source-rate transition with no preceding cost raise must
        // snap the source estimate without falsely blaming frame generation.
        AdaptiveFrameScheduler scheduler(60, 3);
        for (int frame = 0; frame < 4; ++frame)
            scheduler.plan(16ms);
        bool sawRateSnap = false;
        bool sawBackoff = false;
        for (int frame = 0; frame < 3; ++frame) {
            scheduler.plan(34ms);
            sawRateSnap = sawRateSnap || scheduler.telemetry().sourceRateSnapped;
            sawBackoff = sawBackoff || scheduler.telemetry().costBackedOff;
        }
        assert(sawRateSnap);
        assert(!sawBackoff);
    }

    {
        // Discontinuities reset both fractional scheduling and the conservative
        // generation-cost ceiling; a resumed source must probe upward again.
        AdaptiveFrameScheduler scheduler(120, 3);
        assert(scheduler.plan(50ms) == 1);
        assert(scheduler.plan(1s) == 0);
        assert(scheduler.telemetry().discontinuityReset);
        assert(scheduler.telemetry().costLimit == 1);
        assert(scheduler.plan(50ms) == 1);
    }

    {
        AdaptiveFrameScheduler scheduler;
        assert(scheduler.plan(33ms) == 0);
        scheduler.configure(60, 3);
        assert(scheduler.plan(33333333ns) == 1);
        scheduler.configure(90, 3);
        assert(scheduler.plan(33333333ns) == 1);
        assert(scheduler.targetFps() == 90);
    }

    return 0;
}
