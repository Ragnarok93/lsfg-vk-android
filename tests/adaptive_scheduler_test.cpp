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
        // The S25 FE logs contain short present bursts around 100+ FPS while
        // the real game cadence remains near 30 FPS. Three isolated fast
        // samples must not replace the stable source-rate estimate.
        AdaptiveFrameScheduler scheduler(60, 3);
        for (int frame = 0; frame < 12; ++frame)
            scheduler.plan(33333333ns);

        bool snapped = false;
        for (int frame = 0; frame < 3; ++frame) {
            scheduler.plan(9ms);
            snapped = snapped || scheduler.telemetry().sourceRateSnapped;
        }
        scheduler.plan(33333333ns);

        assert(!snapped);
        assert(scheduler.telemetry().smoothedSourceFps < 40.0);
    }

    {
        // A real sustained increase in source rate must still be recognized;
        // it just requires stronger confirmation than a slowdown so transient
        // present bursts cannot zero Adaptive generation.
        AdaptiveFrameScheduler scheduler(60, 3);
        for (int frame = 0; frame < 12; ++frame)
            scheduler.plan(33333333ns);

        bool snapped = false;
        for (int frame = 0; frame < 8; ++frame) {
            scheduler.plan(10ms);
            snapped = snapped || scheduler.telemetry().sourceRateSnapped;
        }

        assert(snapped);
        assert(scheduler.telemetry().smoothedSourceFps > 80.0);
    }

    {
        // A genuine slowdown must remain responsive: three consistent slow
        // intervals should snap the estimate quickly so Adaptive can react to
        // a heavier scene without several seconds of EMA lag.
        AdaptiveFrameScheduler scheduler(60, 3);
        for (int frame = 0; frame < 12; ++frame)
            scheduler.plan(16ms);

        bool snapped = false;
        for (int frame = 0; frame < 3; ++frame) {
            scheduler.plan(34ms);
            snapped = snapped || scheduler.telemetry().sourceRateSnapped;
        }

        assert(snapped);
        assert(scheduler.telemetry().smoothedSourceFps < 35.0);
    }

    {
        // Regression: generated-frame presentation can produce an alternating
        // short/long source cadence (roughly 20 ms / 50 ms on Xclipse 940).
        // Waiting for three *consecutive* slow samples makes the old estimator
        // ignore every 50 ms interval and overestimate source throughput near
        // 40-50 FPS. Adaptive then oscillates between 0/1 generated frames and
        // misses a 60 FPS target. The estimator must account for every valid
        // interval while retaining the burst-confirmation rules above.
        AdaptiveFrameScheduler scheduler(60, 3);
        for (int frame = 0; frame < 12; ++frame)
            scheduler.plan(25ms);

        bool reachedSecondCostLevel = false;
        for (int frame = 0; frame < 40; ++frame) {
            scheduler.plan((frame % 2 == 0) ? 20ms : 50ms);
            reachedSecondCostLevel = reachedSecondCostLevel
                || scheduler.telemetry().costLimit >= 2;
        }

        assert(scheduler.telemetry().smoothedSourceFps < 35.0);
        assert(scheduler.telemetry().wantedGeneratedFrames > 1.0);
        assert(reachedSecondCostLevel);
    }

    {
        // A high target alone must not raise generation cost after only a few
        // frames. Require sustained unmet demand before probing the next level.
        AdaptiveFrameScheduler scheduler(120, 3);
        bool raisedEarly = false;
        for (int frame = 0; frame < 12; ++frame) {
            scheduler.plan(40ms);
            raisedEarly = raisedEarly || scheduler.telemetry().costRaised;
        }
        assert(!raisedEarly);
        assert(scheduler.telemetry().costLimit == 1);

        bool sawRaise = false;
        for (int frame = 0; frame < 8; ++frame) {
            scheduler.plan(40ms);
            sawRaise = sawRaise || scheduler.telemetry().costRaised;
        }
        assert(sawRaise);
        assert(scheduler.telemetry().costLimit == 2);
    }

    {
        // If source FPS collapses shortly after a confirmed cost raise,
        // attribute the correlated drop to framegen and return to the lower
        // cost ceiling.
        AdaptiveFrameScheduler scheduler(120, 3);
        bool sawRaise = false;
        for (int frame = 0; frame < 24; ++frame) {
            scheduler.plan(40ms);
            sawRaise = sawRaise || scheduler.telemetry().costRaised;
            if (sawRaise)
                break;
        }
        assert(sawRaise);

        bool sawBackoff = false;
        for (int frame = 0; frame < 3; ++frame) {
            scheduler.plan(60ms);
            sawBackoff = sawBackoff || scheduler.telemetry().costBackedOff;
        }
        assert(sawBackoff);
        assert(scheduler.telemetry().costLimit == 1);
    }

    {
        // A natural source-rate transition with no preceding cost raise must
        // snap the source estimate without falsely blaming frame generation.
        AdaptiveFrameScheduler scheduler(60, 3);
        for (int frame = 0; frame < 8; ++frame)
            scheduler.plan(16ms);
        bool sawRateSnap = false;
        bool sawBackoff = false;
        bool sawRaise = false;
        for (int frame = 0; frame < 3; ++frame) {
            scheduler.plan(34ms);
            sawRateSnap = sawRateSnap || scheduler.telemetry().sourceRateSnapped;
            sawBackoff = sawBackoff || scheduler.telemetry().costBackedOff;
            sawRaise = sawRaise || scheduler.telemetry().costRaised;
        }
        assert(sawRateSnap);
        assert(!sawBackoff);
        assert(!sawRaise);
    }

    {
        // Discontinuities reset both fractional scheduling and the conservative
        // generation-cost ceiling; a resumed source must re-establish demand
        // before probing upward again.
        AdaptiveFrameScheduler scheduler(120, 3);
        assert(scheduler.plan(50ms) == 1);
        assert(scheduler.plan(1s) == 0);
        assert(scheduler.telemetry().discontinuityReset);
        assert(scheduler.telemetry().costLimit == 1);
        assert(scheduler.plan(50ms) == 1);
        assert(!scheduler.telemetry().costRaised);
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
