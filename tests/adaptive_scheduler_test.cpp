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
        assert(!scheduler.telemetry().configWarmStart);
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
        // Ordinary discontinuities still reset both fractional scheduling and
        // the conservative generation-cost ceiling. Only an explicit config
        // change is allowed to warm-start after a Quick Menu suspension.
        AdaptiveFrameScheduler scheduler(120, 3);
        assert(scheduler.plan(50ms) == 1);
        assert(scheduler.plan(1s) == 0);
        assert(scheduler.telemetry().discontinuityReset);
        assert(scheduler.telemetry().costLimit == 1);
        assert(scheduler.plan(50ms) == 1);
        assert(!scheduler.telemetry().costRaised);
        assert(!scheduler.telemetry().configWarmStart);
    }

    {
        // Regression from the S20+ Quick Menu trace: changing an adaptive
        // target while the guest is suspended used to discard the pause, then
        // cold-start at cost 1 and spend ~0.6 s per level climbing toward the
        // newly requested target. Preserve explicit reconfiguration intent
        // across the pause and seed the first valid cadence sample directly to
        // the bounded cost required by the new target.
        AdaptiveFrameScheduler scheduler(45, 3);
        for (int frame = 0; frame < 12; ++frame)
            scheduler.plan(60ms);

        scheduler.configure(60, 3);
        assert(scheduler.plan(1s) == 0);
        assert(scheduler.telemetry().discontinuityReset);

        const auto generated = scheduler.plan(60ms);
        assert(scheduler.telemetry().configWarmStart);
        assert(scheduler.telemetry().wantedGeneratedFrames > 2.5);
        assert(scheduler.telemetry().costLimit == 3);
        assert(generated >= 2);
    }

    {
        // Actual S20+ ordering from the 2026-09-17 trace is the inverse: the
        // first old-config present after resume consumes the suspend-spanning
        // discontinuity, and only then does the layer notice conf.toml changed.
        // A target change after that discontinuity must still remember that
        // this is an established runtime and warm-start the next valid sample.
        AdaptiveFrameScheduler scheduler(60, 3);
        for (int frame = 0; frame < 12; ++frame)
            scheduler.plan(60ms);

        assert(scheduler.plan(1s) == 0);
        assert(scheduler.telemetry().discontinuityReset);
        scheduler.configure(90, 3);

        const auto generated = scheduler.plan(60ms);
        assert(scheduler.telemetry().configWarmStart);
        assert(scheduler.telemetry().wantedGeneratedFrames == 3.0);
        assert(scheduler.telemetry().costLimit == 3);
        assert(generated >= 2);
    }

    {
        // The warm start must remain fail-safe: if the newly seeded load causes
        // a prompt source-rate regression, the existing blame window must back
        // it off rather than pinning the user-selected target at an unsafe cost.
        AdaptiveFrameScheduler scheduler(45, 3);
        for (int frame = 0; frame < 12; ++frame)
            scheduler.plan(40ms);
        scheduler.configure(120, 3);
        assert(scheduler.plan(1s) == 0);
        scheduler.plan(40ms);
        assert(scheduler.telemetry().configWarmStart);
        assert(scheduler.telemetry().costLimit == 3);
        scheduler.plan(80ms);
        assert(scheduler.telemetry().costBackedOff);
        assert(scheduler.telemetry().costLimit == 2);
    }

    {
        // First-time configuration is still a cold start; merely constructing
        // or enabling Adaptive must not bypass the established safety ramp.
        AdaptiveFrameScheduler scheduler;
        assert(scheduler.plan(33ms) == 0);
        scheduler.configure(60, 3);
        assert(scheduler.plan(60ms) == 1);
        assert(!scheduler.telemetry().configWarmStart);
        scheduler.configure(90, 3);
        assert(scheduler.plan(60ms) >= 1);
        assert(scheduler.telemetry().configWarmStart);
        assert(scheduler.targetFps() == 90);
    }

    {
        // An explicit lifecycle reset is different from a suspend cadence
        // discontinuity. It must erase established-runtime history so a later
        // configuration behaves like a true cold start.
        AdaptiveFrameScheduler scheduler(60, 3);
        for (int frame = 0; frame < 8; ++frame)
            scheduler.plan(40ms);
        scheduler.reset();
        scheduler.configure(90, 3);
        assert(scheduler.plan(60ms) == 1);
        assert(!scheduler.telemetry().configWarmStart);
    }

    {
        // A generation ceiling that was once affordable must not remain pinned
        // after later GPU contention collapses the real/source cadence. Probe a
        // lower interpolation cost before adding yet more generated work.
        AdaptiveFrameScheduler scheduler(60, 3);
        for (int frame = 0; frame < 48; ++frame)
            scheduler.plan(40ms);
        assert(scheduler.telemetry().costLimit >= 2);

        bool sawProtectiveProbe = false;
        for (int frame = 0; frame < 30; ++frame) {
            scheduler.plan(55ms);
            sawProtectiveProbe = sawProtectiveProbe
                || (scheduler.telemetry().costBackedOff
                    && scheduler.telemetry().costProbe);
            if (sawProtectiveProbe)
                break;
        }
        assert(sawProtectiveProbe);
        assert(scheduler.telemetry().costLimit == 1);

        // If source cadence recovers enough that the lower-cost operating point
        // preserves almost the same output throughput, keep the cheaper level.
        for (int frame = 0; frame < 40; ++frame)
            scheduler.plan(35ms);
        assert(scheduler.telemetry().costLimit == 1);
    }

    {
        // A source-preservation probe is causal, not a blind downgrade. If
        // lowering generation cost does not recover source FPS, restore the
        // previous interpolation level instead of sacrificing output for no gain.
        AdaptiveFrameScheduler scheduler(60, 3);
        for (int frame = 0; frame < 48; ++frame)
            scheduler.plan(40ms);
        assert(scheduler.telemetry().costLimit >= 2);

        bool sawProtectiveProbe = false;
        for (int frame = 0; frame < 30; ++frame) {
            scheduler.plan(55ms);
            sawProtectiveProbe = sawProtectiveProbe
                || (scheduler.telemetry().costBackedOff
                    && scheduler.telemetry().costProbe);
            if (sawProtectiveProbe)
                break;
        }
        assert(sawProtectiveProbe);

        bool restored = false;
        for (int frame = 0; frame < 40; ++frame) {
            scheduler.plan(55ms);
            restored = restored
                || (scheduler.telemetry().costRaised
                    && scheduler.telemetry().costProbe);
            if (restored)
                break;
        }
        assert(restored);
        assert(scheduler.telemetry().costLimit >= 2);
    }

    {
        // AFG must not respond to a sub-30 FPS source collapse by increasing
        // interpolation load. At 20 FPS toward 60, attenuated demand is 1.0
        // generated frame per real frame instead of the raw 2.0.
        AdaptiveFrameScheduler scheduler(60, 3);
        std::size_t generated = 0;
        for (int frame = 0; frame < 30; ++frame)
            generated += scheduler.plan(50ms);
        assert(scheduler.telemetry().smoothedSourceFps > 19.0);
        assert(scheduler.telemetry().smoothedSourceFps < 21.0);
        assert(scheduler.telemetry().wantedGeneratedFrames <= 1.01);
        assert(generated <= 31);
    }

    {
        // Below 30 FPS, interpolation pressure scales down continuously. A
        // 15 FPS source toward 60 should average roughly 0.75 generated frames
        // per real frame rather than requesting the maximum 3x synthetic load.
        AdaptiveFrameScheduler scheduler(60, 3);
        std::size_t generated = 0;
        for (int frame = 0; frame < 40; ++frame)
            generated += scheduler.plan(66666667ns);
        assert(scheduler.telemetry().smoothedSourceFps > 14.0);
        assert(scheduler.telemetry().smoothedSourceFps < 16.0);
        assert(scheduler.telemetry().wantedGeneratedFrames < 0.80);
        assert(generated >= 27 && generated <= 32);
    }

    {
        // Hard safety cutoff: at or below 10 FPS AFG must generate nothing,
        // and suppressed fractional debt must not burst when source cadence
        // recovers.
        AdaptiveFrameScheduler scheduler(60, 3);
        for (int frame = 0; frame < 12; ++frame)
            assert(scheduler.plan(100ms) == 0);
        assert(scheduler.telemetry().wantedGeneratedFrames == 0.0);

        const auto firstRecovered = scheduler.plan(33333333ns);
        assert(firstRecovered <= 1);
    }

    return 0;
}
