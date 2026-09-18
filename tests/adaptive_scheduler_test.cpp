#include "adaptive_scheduler.hpp"

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cmath>
#include <vector>

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
        // Fractional density is represented as explicit opportunities on one
        // continuous phase lattice, not by switching integer multiplier modes.
        // A 0.5 synthetic/source density should therefore produce one
        // opportunity every two protected source intervals at a stable phase.
        AdaptiveFrameScheduler scheduler(75, 3);
        std::vector<double> absoluteSlots;
        for (int frame = 0; frame < 12; ++frame) {
            const auto plan = scheduler.planSlots(20ms);
            assert(std::abs(plan.desiredDensity - 0.5) < 0.0001);
            assert(std::abs(plan.governedDensity - 0.5) < 0.0001);
            for (const double phase : plan.slotPhases) {
                assert(phase > 0.0 && phase <= 1.0);
                absoluteSlots.push_back(static_cast<double>(frame) + phase);
            }
        }
        assert(absoluteSlots.size() == 6);
        for (std::size_t i = 1; i < absoluteSlots.size(); ++i)
            assert(std::abs((absoluteSlots[i] - absoluteSlots[i - 1]) - 2.0) < 0.0001);
    }

    {
        // Consuming/rejecting an opportunity is deliberately external to the
        // scheduler. Merely ignoring a created slot must not cause a later
        // catch-up burst or alter the phase lattice.
        AdaptiveFrameScheduler scheduler(75, 3);
        std::vector<std::size_t> counts;
        for (int frame = 0; frame < 8; ++frame)
            counts.push_back(scheduler.planSlots(20ms).slotPhases.size());

        for (const auto count : counts)
            assert(count <= 1);
        std::size_t total = 0;
        for (const auto count : counts)
            total += count;
        assert(total == 4);
        assert(scheduler.telemetry().fractionalPhase >= 0.0);
        assert(scheduler.telemetry().fractionalPhase < 1.0);
    }


    {
        // Source timing is indexed only by real-frame arrivals. Synthetic slot
        // queries cannot advance or re-phase the next real-frame deadline.
        SourceFrameTimeline timeline;
        const auto first = timeline.observe(1'000'000'000ULL, 20ms);
        assert(first.valid);
        assert(first.sourceIndex == 0);
        assert(first.anchorNs == 1'000'000'000ULL);
        assert(first.sourceDeadlineNs == 1'020'000'000ULL);
        const auto midpoint = first.syntheticDeadlineNs(0.5);
        assert(midpoint == 1'010'000'000ULL);
        assert(first.sourceDeadlineNs == 1'020'000'000ULL);

        const auto second = timeline.observe(1'020'000'000ULL, 20ms);
        assert(second.valid);
        assert(second.sourceIndex == 1);
        assert(second.anchorNs == 1'020'000'000ULL);
        assert(second.sourceDeadlineNs == 1'040'000'000ULL);
    }

    {
        // A source discontinuity invalidates only the presentation epoch. It
        // must not manufacture historical synthetic timestamps or catch-up.
        SourceFrameTimeline timeline;
        assert(timeline.observe(2'000'000'000ULL, 16ms).valid);
        assert(!timeline.observe(3'000'000'000ULL, 300ms).valid);
        const auto resumed = timeline.observe(3'020'000'000ULL, 20ms);
        assert(resumed.valid);
        assert(resumed.sourceIndex == 0);
        assert(resumed.sourceDeadlineNs > resumed.anchorNs);
    }


    {
        // Deadline admission is a stateless fast decision, not another slow
        // governor. The same source cycle and cost model must yield per-slot
        // decisions solely from remaining presentation budget.
        SourceTimelineCycle cycle{
            .valid = true,
            .sourceIndex = 4,
            .anchorNs = 1'000'000'000ULL,
            .intervalNs = 20'000'000ULL,
            .sourceDeadlineNs = 1'020'000'000ULL,
        };
        const std::vector<double> phases{0.25, 0.75};
        const auto roomy = SyntheticDeadlineAdmission::evaluate(
            1'000'000'000ULL, cycle, phases, 2.0, 1.0, 0.0);
        assert(roomy.predictionValid);
        assert(roomy.rejectedCount == 0);
        assert(roomy.slots.size() == 2);
        assert(roomy.slots[0].admitted);
        assert(roomy.slots[1].admitted);

        const auto pressured = SyntheticDeadlineAdmission::evaluate(
            1'004'500'000ULL, cycle, phases, 2.0, 1.0, 0.0);
        assert(pressured.predictionValid);
        assert(pressured.rejectedCount == 1);
        assert(!pressured.slots[0].admitted);
        assert(pressured.slots[1].admitted);
    }

    {
        // With no cost history, future slots fail open; a slot whose deadline
        // already passed is still consumed as missed rather than carried as debt.
        SourceTimelineCycle cycle{
            .valid = true,
            .sourceIndex = 8,
            .anchorNs = 2'000'000'000ULL,
            .intervalNs = 40'000'000ULL,
            .sourceDeadlineNs = 2'040'000'000ULL,
        };
        const std::vector<double> phases{0.25, 0.75};
        const auto unknown = SyntheticDeadlineAdmission::evaluate(
            2'000'000'000ULL, cycle, phases, 0.0, 0.0, 0.0);
        assert(!unknown.predictionValid);
        assert(unknown.rejectedCount == 0);

        const auto missed = SyntheticDeadlineAdmission::evaluate(
            2'020'000'000ULL, cycle, phases, 0.0, 0.0, 0.0);
        assert(!missed.predictionValid);
        assert(missed.rejectedCount == 1);
        assert(!missed.slots[0].admitted);
        assert(missed.slots[1].admitted);
    }


    return 0;
}
