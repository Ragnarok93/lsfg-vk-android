#include "adaptive_scheduler.hpp"

#include <cassert>
#include <chrono>
#include <cstddef>

using namespace std::chrono_literals;

int main() {
    {
        // Source deadlines advance once per real source observation. Querying
        // synthetic positions cannot advance or re-phase the protected source
        // timeline.
        SourceProtectedTimeline timeline;
        const auto first = timeline.observe(1'000'000'000ULL, 16ms);
        assert(first.valid);
        assert(first.rebased);
        assert(first.sourceIndex == 0);
        assert(first.sourceDesiredTimeNs == 1'016'000'000ULL);

        const auto quarter =
            timeline.syntheticDesiredTimeNs(first, 0.25);
        const auto threeQuarter =
            timeline.syntheticDesiredTimeNs(first, 0.75);
        assert(quarter == 1'004'000'000ULL);
        assert(threeQuarter == 1'012'000'000ULL);

        const auto second = timeline.observe(1'016'000'000ULL, 16ms);
        assert(second.sourceIndex == 1);
        assert(second.previousSourceDesiredTimeNs == first.sourceDesiredTimeNs);
        assert(second.sourceDesiredTimeNs == 1'032'000'000ULL);
        assert(second.sourceDeadlineErrorNs == 0);

        // A synthetic opportunity can be ignored/rejected without changing the
        // next source deadline because no generated-work feedback enters observe().
        (void) timeline.syntheticDesiredTimeNs(second, 0.5);
        const auto third = timeline.observe(1'032'000'000ULL, 16ms);
        assert(third.sourceDesiredTimeNs == 1'048'000'000ULL);
    }

    {
        // Phase follows the real source arrival, while the future generation
        // window follows predicted cadence rather than the lateness itself.
        SourceProtectedTimeline timeline;
        const auto first = timeline.observe(2'000'000'000ULL, 20ms);
        assert(first.valid);
        const auto late = timeline.observe(2'050'000'000ULL, 20ms);
        assert(late.rebased);
        assert(late.sourceDeadlineErrorNs == 30'000'000LL);
        assert(late.previousSourceDesiredTimeNs == 2'050'000'000ULL);
        assert(late.intervalNs == 20'000'000ULL);
        assert(late.sourceDesiredTimeNs == 2'070'000'000ULL);
    }

    {
        // A single slow frame cannot become a giant synthetic budget. The
        // source arrival is authoritative for phase, while cadence expands only
        // gradually from the previous real-source prediction.
        SourceProtectedTimeline timeline;
        const auto initial = timeline.observe(5'000'000'000ULL, 16ms);
        assert(initial.valid);
        assert(initial.intervalNs == 16'000'000ULL);
        assert(initial.sourceDesiredTimeNs == 5'016'000'000ULL);

        const auto slowSpike = timeline.observe(5'050'000'000ULL, 50ms);
        assert(slowSpike.valid);
        assert(slowSpike.rebased);
        assert(slowSpike.sourceDeadlineErrorNs == 34'000'000LL);
        assert(slowSpike.previousSourceDesiredTimeNs == 5'050'000'000ULL);
        // Upward prediction is capped to +10% per source observation:
        // bounded observation=24ms, alpha=.2 => 17.6ms.
        assert(slowSpike.intervalNs == 17'600'000ULL);
        assert(slowSpike.sourceDesiredTimeNs == 5'067'600'000ULL);

        const auto sustainedSlow =
            timeline.observe(5'100'000'000ULL, 50ms);
        assert(sustainedSlow.valid);
        assert(sustainedSlow.intervalNs == 19'360'000ULL);
        assert(sustainedSlow.sourceDesiredTimeNs == 5'119'360'000ULL);
    }

    {
        // When the source speeds up, the prediction contracts much faster so
        // frame generation cannot keep spending against an obsolete long
        // interval.
        SourceProtectedTimeline timeline;
        assert(timeline.observe(6'000'000'000ULL, 32ms).valid);

        const auto fast =
            timeline.observe(6'016'000'000ULL, 16ms);
        assert(fast.valid);
        // 32ms -> bounded 16ms, alpha=.5 => 24ms.
        assert(fast.intervalNs == 24'000'000ULL);
        assert(fast.previousSourceDesiredTimeNs == 6'016'000'000ULL);
        assert(fast.sourceDesiredTimeNs == 6'040'000'000ULL);

        const auto faster =
            timeline.observe(6'032'000'000ULL, 16ms);
        assert(faster.valid);
        assert(faster.intervalNs == 20'000'000ULL);
        assert(faster.sourceDesiredTimeNs == 6'052'000'000ULL);
    }

    {
        // Small source jitter changes the measured error but never shifts the
        // synthetic interval start away from the real source arrival.
        SourceProtectedTimeline timeline;
        assert(timeline.observe(7'000'000'000ULL, 16ms).valid);

        const auto jitter =
            timeline.observe(7'016'500'000ULL, 16'500'000ns);
        assert(jitter.valid);
        assert(!jitter.rebased);
        assert(jitter.previousSourceDesiredTimeNs == 7'016'500'000ULL);
        assert(jitter.sourceDesiredTimeNs > jitter.previousSourceDesiredTimeNs);
        const auto midpoint = timeline.syntheticDesiredTimeNs(jitter, 0.5);
        assert(midpoint > jitter.previousSourceDesiredTimeNs);
        assert(midpoint < jitter.sourceDesiredTimeNs);
    }

    {
        // Source timeline discontinuities are cadence-relative in every mode.
        // A suspend-like outlier must not create a historical synthetic span or
        // a delayed source deadline, while a slow-but-stable cadence remains valid.
        SourceProtectedTimeline timeline;
        assert(timeline.observe(3'000'000'000ULL, 16ms).valid);
        assert(timeline.observe(3'016'000'000ULL, 16ms).valid);

        const auto suspended =
            timeline.observe(3'516'000'000ULL, 500ms);
        assert(!suspended.valid);

        const auto resumed =
            timeline.observe(3'532'000'000ULL, 16ms);
        assert(resumed.valid);
        assert(resumed.rebased);
        assert(resumed.sourceIndex == 0);
        assert(resumed.sourceDesiredTimeNs == 3'548'000'000ULL);

        SourceProtectedTimeline slowTimeline;
        assert(slowTimeline.observe(4'000'000'000ULL, 125ms).valid);
        assert(slowTimeline.observe(4'125'000'000ULL, 125ms).valid);
    }

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
        // Sustained faster cadence is recognized through the robust window and
        // bounded EMA, not a hard snap. Short WSI bursts therefore cannot erase
        // interpolation demand, while a real transition still converges.
        AdaptiveFrameScheduler scheduler(60, 3);
        for (int frame = 0; frame < 12; ++frame)
            scheduler.plan(33333333ns);

        for (int frame = 0; frame < 8; ++frame)
            scheduler.plan(10ms);
        assert(!scheduler.telemetry().sourceRateSnapped);
        assert(scheduler.telemetry().smoothedSourceFps > 40.0);
        assert(scheduler.telemetry().smoothedSourceFps < 70.0);

        for (int frame = 0; frame < 16; ++frame)
            scheduler.plan(10ms);
        assert(scheduler.telemetry().smoothedSourceFps > 80.0);
    }

    {
        // Sustained slowdown must remain responsive without allowing one bursty
        // interval to replace the source baseline.
        AdaptiveFrameScheduler scheduler(60, 3);
        for (int frame = 0; frame < 12; ++frame)
            scheduler.plan(16ms);

        scheduler.plan(90ms);
        assert(scheduler.telemetry().smoothedSourceFps > 50.0);
        assert(scheduler.telemetry().syntheticOpportunitiesCreated <= 1);

        for (int frame = 0; frame < 8; ++frame)
            scheduler.plan(34ms);
        assert(!scheduler.telemetry().sourceRateSnapped);
        assert(scheduler.telemetry().smoothedSourceFps < 35.0);
    }

    {
        // Robust cadence changes must not erase sustained unmet-demand evidence.
        // At 60 FPS target, a stable ~29 FPS source still reaches cost level 2.
        AdaptiveFrameScheduler scheduler(60, 3);
        for (int frame = 0; frame < 12; ++frame)
            scheduler.plan(16ms);

        bool sawRaise = false;
        for (int frame = 0; frame < 32; ++frame) {
            scheduler.plan(34ms);
            sawRaise = sawRaise || scheduler.telemetry().costRaised;
        }

        assert(sawRaise);
        assert(scheduler.telemetry().costLimit >= 2);
        assert(scheduler.telemetry().wantedGeneratedFrames > 1.0);
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
        for (int frame = 0; frame < 6; ++frame) {
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
        bool sawBackoff = false;
        bool sawRaise = false;
        for (int frame = 0; frame < 3; ++frame) {
            scheduler.plan(34ms);
            sawBackoff = sawBackoff || scheduler.telemetry().costBackedOff;
            sawRaise = sawRaise || scheduler.telemetry().costRaised;
        }
        assert(!scheduler.telemetry().sourceRateSnapped);
        assert(!sawBackoff);
        assert(!sawRaise);
    }

    {
        // Build #386 regression: an ordinary source-timeline discontinuity may
        // clear interpolation debt/cadence windows, but it must not erase a
        // generation ceiling that already survived causal validation. After
        // warmup the scheduler should resume that proven ceiling immediately.
        AdaptiveFrameScheduler scheduler(120, 3);
        for (int frame = 0; frame < 120; ++frame) {
            scheduler.setSafeGenerationHint(3, true);
            scheduler.plan(33ms);
        }
        assert(scheduler.telemetry().costLimit == 3);
        assert(scheduler.telemetry().provenCostLimit == 3);

        assert(scheduler.plan(1s) == 0);
        assert(scheduler.telemetry().discontinuityReset);

        scheduler.setSafeGenerationHint(3, true);
        scheduler.plan(33ms);
        assert(scheduler.telemetry().costLimit == 3);
        assert(!scheduler.telemetry().costBackedOff);
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
        bool backedOff = false;
        for (int frame = 0; frame < 6 && !backedOff; ++frame) {
            scheduler.plan(80ms);
            backedOff = scheduler.telemetry().costBackedOff;
        }
        assert(backedOff);
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
        // A failed source-preservation probe must not repeat every ~0.6 s.
        // Once the cheaper level fails to recover enough cadence and the
        // original cost is restored, hold that causal experiment for several
        // seconds before allowing another 2 -> 1 probe.
        AdaptiveFrameScheduler scheduler(60, 3);
        for (int frame = 0; frame < 48; ++frame)
            scheduler.plan(40ms);
        assert(scheduler.telemetry().costLimit >= 2);

        bool backedOff = false;
        for (int frame = 0; frame < 30 && !backedOff; ++frame) {
            scheduler.plan(55ms);
            backedOff = scheduler.telemetry().costBackedOff
                && scheduler.telemetry().costProbe;
        }
        assert(backedOff);

        bool restored = false;
        for (int frame = 0; frame < 40 && !restored; ++frame) {
            scheduler.plan(55ms);
            restored = scheduler.telemetry().costRaised
                && scheduler.telemetry().costProbe;
        }
        assert(restored);
        const auto restoredCost = scheduler.telemetry().costLimit;
        assert(restoredCost >= 2);

        bool repeatedProbe = false;
        for (int frame = 0; frame < 70; ++frame) { // 3.85 s
            scheduler.plan(55ms);
            repeatedProbe = repeatedProbe
                || (scheduler.telemetry().costBackedOff
                    && scheduler.telemetry().costProbe);
        }
        assert(!repeatedProbe);
        assert(scheduler.telemetry().costLimit == restoredCost);
    }

    {
        // Build #386 regression: a healthy proven 4x-capable operating point
        // must survive a nearby target change. The target change may reset the
        // fractional lattice, but unchanged source cadence plus a safe hint of
        // three must not collapse the generation ceiling from 3 to 1/2.
        AdaptiveFrameScheduler scheduler(90, 3);
        for (int frame = 0; frame < 120; ++frame) {
            scheduler.setSafeGenerationHint(3, true);
            scheduler.plan(34ms);
        }
        assert(scheduler.telemetry().costLimit == 3);
        assert(scheduler.telemetry().provenCostLimit == 3);

        scheduler.configure(95, 3);
        assert(scheduler.plan(1s) == 0);
        bool backedOff = false;
        for (int frame = 0; frame < 45; ++frame) {
            scheduler.setSafeGenerationHint(3, true);
            scheduler.plan(34ms);
            backedOff = backedOff || scheduler.telemetry().costBackedOff;
        }
        assert(!backedOff);
        assert(scheduler.telemetry().costLimit == 3);
    }

    {
        // Target deficit is demand for more generated work, not proof that the
        // current generation level is harming source cadence. With a stable
        // source and a predictor that says the next level fits, the scheduler
        // must climb 1 -> 2 -> 3 instead of source-preservation probing 2 -> 1.
        AdaptiveFrameScheduler scheduler(120, 3);
        bool preservationBackoff = false;
        for (int frame = 0; frame < 70; ++frame) {
            scheduler.setSafeGenerationHint(3, true);
            scheduler.plan(33ms);
            preservationBackoff = preservationBackoff
                || (scheduler.telemetry().costBackedOff
                    && scheduler.telemetry().costProbe);
        }
        assert(!preservationBackoff);
        assert(scheduler.telemetry().costLimit == 3);
    }

    {
        // A causal backoff must never become a permanent recovery lock. After
        // the lower level observes a stable recovered source cadence and the
        // predictor again supports the next level, persistent unmet demand must
        // produce another probe within a bounded interval.
        AdaptiveFrameScheduler scheduler(120, 3);
        bool promoted = false;
        for (int frame = 0; frame < 30 && !promoted; ++frame) {
            scheduler.setSafeGenerationHint(2, true);
            scheduler.plan(40ms);
            promoted = scheduler.telemetry().costLimit >= 2;
        }
        assert(promoted);

        bool backedOff = false;
        for (int frame = 0; frame < 10 && !backedOff; ++frame) {
            scheduler.setSafeGenerationHint(2, true);
            scheduler.plan(65ms);
            backedOff = scheduler.telemetry().costBackedOff;
        }
        assert(backedOff);
        assert(scheduler.telemetry().costLimit == 1);

        bool retried = false;
        for (int frame = 0; frame < 70 && !retried; ++frame) {
            scheduler.setSafeGenerationHint(2, true);
            scheduler.plan(40ms);
            retried = scheduler.telemetry().costRaised
                && scheduler.telemetry().costProbe;
        }
        assert(retried);
        assert(scheduler.telemetry().costLimit >= 2);
    }

    {
        // Source rate by itself is never a reason to disable interpolation.
        // A slow but stable source remains eligible under the same generation
        // semantics as every other cadence.
        AdaptiveFrameScheduler scheduler(60, 3);
        for (int frame = 0; frame < 12; ++frame) {
            assert(scheduler.plan(125ms) > 0); // stable 8 FPS source
            assert(!scheduler.telemetry().discontinuityReset);
        }
    }

    {
        // Fractional density creates deterministic opportunities rather than
        // changing a coarse multiplier mode. Ignoring an opportunity is not
        // fed back into the distributor, so it cannot become catch-up debt.
        AdaptiveFrameScheduler scheduler(60, 3);
        std::size_t opportunities = 0;
        std::size_t priorOpportunityFrame = 0;
        std::size_t maxGap = 0;
        for (std::size_t frame = 1; frame <= 20; ++frame) {
            const auto created = scheduler.plan(20ms); // ~0.2 generated/source
            assert(created <= 1);
            assert(scheduler.telemetry().syntheticOpportunitiesCreated == created);
            assert(scheduler.telemetry().fractionalPhase >= 0.0);
            assert(scheduler.telemetry().fractionalPhase < 1.0);
            if (created != 0) {
                if (priorOpportunityFrame != 0)
                    maxGap = std::max(maxGap, frame - priorOpportunityFrame);
                priorOpportunityFrame = frame;
                opportunities += created;
                // Deliberately pretend downstream rejected this opportunity.
                // The next plan() call receives no rejection/debt feedback.
            }
        }
        assert(opportunities >= 3 && opportunities <= 5);
        assert(maxGap <= 6);
    }


    {
        // Fluidity regression: a mixed 20/50 ms source cadence should allocate
        // synthetic work to the long intervals instead of deriving every count
        // from the lagging smoothed-rate estimate. Once cost level 2 is proven
        // sustainable, 50 ms intervals should receive two opportunities while
        // 20 ms intervals remain at zero/one. This minimizes local output-gap
        // variance without delaying real source frames.
        AdaptiveFrameScheduler scheduler(60, 3);
        for (int frame = 0; frame < 48; ++frame)
            scheduler.plan(35ms);
        assert(scheduler.telemetry().costLimit >= 2);

        std::size_t longTwo = 0;
        std::size_t shortOverOne = 0;
        std::size_t totalGenerated = 0;
        for (int frame = 0; frame < 40; ++frame) {
            const bool longInterval = (frame % 2) != 0;
            const auto generated =
                scheduler.plan(longInterval ? 50ms : 20ms);
            totalGenerated += generated;
            if (longInterval && generated == 2)
                ++longTwo;
            if (!longInterval && generated > 1)
                ++shortOverOne;
        }

        assert(longTwo >= 12);
        assert(shortOverOne == 0);
        assert(totalGenerated >= 34);
        assert(totalGenerated <= 46);
        assert(scheduler.telemetry().fractionalPhase >= 0.0);
        assert(scheduler.telemetry().fractionalPhase < 1.0);
    }

    {
        // Capacity-informed promotion may advance exactly one level before the
        // generic 600 ms timer, but only after a time-covered stable source
        // baseline and consecutive safe hints.
        AdaptiveFrameScheduler scheduler(120, 3);
        bool promoted = false;
        for (int frame = 0; frame < 14; ++frame) {
            scheduler.setSafeGenerationHint(2, true);
            scheduler.plan(40ms);
            promoted = promoted || scheduler.telemetry().capacityPromoted;
        }
        assert(promoted);
        assert(scheduler.telemetry().costLimit == 2);

        // The existing causal veto remains authoritative after an early raise,
        // but a single hitch is not enough to blame the new generation level.
        bool backedOff = false;
        for (int frame = 0; frame < 6 && !backedOff; ++frame) {
            scheduler.setSafeGenerationHint(2, true);
            scheduler.plan(80ms);
            backedOff = scheduler.telemetry().costBackedOff;
        }
        assert(backedOff);
        assert(scheduler.telemetry().costLimit == 1);
    }

    {
        // Build #383 regression: predictor capacity alone must not early-promote
        // while the recent real-source cadence is still alternating wildly.
        // The generic sustained-demand timer remains available, but the fast
        // capacity path requires a stable cadence window.
        AdaptiveFrameScheduler scheduler(120, 3);
        const std::array<std::chrono::milliseconds, 4> unstable{
            20ms, 60ms, 20ms, 60ms,
        };
        bool promoted = false;
        for (const auto interval : unstable) {
            scheduler.setSafeGenerationHint(2, true);
            scheduler.plan(interval);
            promoted = promoted || scheduler.telemetry().capacityPromoted;
        }
        assert(!promoted);
        assert(scheduler.telemetry().costLimit == 1);

        // After an unstable period, baseline qualification deliberately
        // forfeits the early shortcut. Once cadence is stable, the ordinary
        // sustained-demand path must still converge to the supported level.
        bool reachedSecondLevel = false;
        for (int frame = 0; frame < 14 && !reachedSecondLevel; ++frame) {
            scheduler.setSafeGenerationHint(2, true);
            scheduler.plan(40ms);
            reachedSecondLevel = scheduler.telemetry().costLimit >= 2;
        }
        assert(reachedSecondLevel);
        assert(scheduler.telemetry().costLimit == 2);
    }

    {
        // The causal veto after a capacity promotion also needs sustained
        // degradation. One or two post-raise slow samples are insufficient to
        // blame frame generation; a continuing regression still backs off.
        AdaptiveFrameScheduler scheduler(120, 3);
        bool promoted = false;
        for (int frame = 0; frame < 14 && !promoted; ++frame) {
            scheduler.setSafeGenerationHint(2, true);
            scheduler.plan(40ms);
            promoted = promoted || scheduler.telemetry().capacityPromoted;
        }
        assert(promoted);
        assert(scheduler.telemetry().costLimit == 2);

        bool backedOff = false;
        for (int frame = 0; frame < 2; ++frame) {
            scheduler.setSafeGenerationHint(2, true);
            scheduler.plan(80ms);
            backedOff = backedOff || scheduler.telemetry().costBackedOff;
        }
        assert(!backedOff);
        assert(scheduler.telemetry().costLimit == 2);

        for (int frame = 0; frame < 6 && !backedOff; ++frame) {
            scheduler.setSafeGenerationHint(2, true);
            scheduler.plan(80ms);
            backedOff = scheduler.telemetry().costBackedOff;
        }
        assert(backedOff);
        assert(scheduler.telemetry().costLimit == 1);
    }

    {
        // #386 capacity-evidence regression: a predictor that continues to
        // certify the promoted level is contradictory evidence against blaming
        // a modest source-rate dip on generated work. Preserve that level
        // unless the source loss is materially deeper; severe collapse still
        // remains eligible for causal backoff.
        AdaptiveFrameScheduler scheduler(120, 3);
        bool promoted = false;
        for (int frame = 0; frame < 20 && !promoted; ++frame) {
            scheduler.setSafeGenerationHint(2, true);
            scheduler.plan(33ms);
            promoted = scheduler.telemetry().costLimit >= 2;
        }
        assert(promoted);

        bool backedOff = false;
        for (int frame = 0; frame < 18; ++frame) {
            scheduler.setSafeGenerationHint(2, true);
            scheduler.plan(40ms);
            backedOff = backedOff || scheduler.telemetry().costBackedOff;
        }
        assert(!backedOff);
        assert(scheduler.telemetry().costLimit == 2);
    }

    {
        // Build #385 regression: post-promotion source settling must be judged
        // over elapsed time, not a handful of source samples. Roughly 0.3 s of
        // temporary slow cadence is insufficient to blame the newly promoted
        // generation level, and recovery must clear that transient evidence.
        AdaptiveFrameScheduler scheduler(120, 3);
        bool promoted = false;
        for (int frame = 0; frame < 14 && !promoted; ++frame) {
            scheduler.setSafeGenerationHint(2, true);
            scheduler.plan(40ms);
            promoted = promoted || scheduler.telemetry().capacityPromoted;
        }
        assert(promoted);
        assert(scheduler.telemetry().costLimit == 2);

        bool backedOff = false;
        for (int frame = 0; frame < 4; ++frame) {
            scheduler.setSafeGenerationHint(2, true);
            scheduler.plan(80ms);
            backedOff = backedOff || scheduler.telemetry().costBackedOff;
        }
        assert(!backedOff);
        assert(scheduler.telemetry().costLimit == 2);

        for (int frame = 0; frame < 10; ++frame) {
            scheduler.setSafeGenerationHint(2, true);
            scheduler.plan(40ms);
            backedOff = backedOff || scheduler.telemetry().costBackedOff;
        }
        assert(!backedOff);
        assert(scheduler.telemetry().costLimit == 2);
    }

    {
        // A single long-but-not-discontinuous source hitch is consumed without
        // minting several target slots or catch-up debt.
        AdaptiveFrameScheduler scheduler(60, 3);
        for (int frame = 0; frame < 12; ++frame)
            scheduler.plan(16ms);
        const auto hitch = scheduler.plan(100ms);
        assert(hitch <= 1);
        assert(scheduler.telemetry().opportunityIntervalSeconds < 0.030);
        const auto next = scheduler.plan(16ms);
        assert(next <= 1);
    }

    {
        // WSI capacity is independent of scheduler cost. Sustained rejection
        // lowers one presentation level; recovery requires a much longer clean
        // run and never blocks or pre-acquires a swapchain image.
        GeneratedPresentationCapacityTracker capacity;
        capacity.configure(3);
        assert(capacity.limit(3) == 3);
        for (int i = 0; i < 3; ++i)
            capacity.observe(2, 1);
        assert(capacity.telemetry().generationCap == 2);
        assert(capacity.telemetry().pressure);

        for (int i = 0; i < 24; ++i)
            capacity.observe(2, 0);
        assert(capacity.telemetry().generationCap == 3);
        assert(capacity.telemetry().raised);
    }

    {
        // Build #383 regression: intermittent-but-sustained WSI rejection must
        // accumulate presentation-pressure evidence instead of being erased by
        // each clean attempt.
        GeneratedPresentationCapacityTracker capacity;
        capacity.configure(2);
        for (int i = 0;
                i < 12 && capacity.telemetry().generationCap > 1;
                ++i) {
            assert(capacity.limit(2) >= 1);
            capacity.observe(2, (i % 2 == 0) ? 1 : 0);
        }
        assert(capacity.telemetry().generationCap == 1);
    }

    {
        // A cap of one is not enough when WSI cannot accept even one synthetic
        // frame per source cycle. Sustained rejection at generationCap==1 must
        // deterministically suppress some single-frame opportunities so the
        // expensive work is skipped before dispatch, while retaining probe
        // attempts that can later demonstrate recovery.
        GeneratedPresentationCapacityTracker capacity;
        capacity.configure(1);

        bool suppressed = false;
        bool probeObserved = false;
        for (int i = 0; i < 24; ++i) {
            const auto allowed = capacity.limit(1);
            if (allowed == 0) {
                suppressed = true;
                continue;
            }
            probeObserved = true;
            capacity.observe(1, 1);
        }

        assert(suppressed);
        assert(probeObserved);
        assert(capacity.telemetry().pressure);
    }

    {
        // Build #386 regression: pressure evidence used to disappear at every
        // integer cap reduction, so real intermittent WSI pressure could reach
        // cap==1 yet never enter sub-one duty. Preserve enough pressure evidence
        // across 3 -> 2 -> 1 that one further rejected single-frame probe can
        // lower duty before more doomed work is dispatched.
        GeneratedPresentationCapacityTracker capacity;
        capacity.configure(3);
        capacity.observe(3, 1);
        capacity.observe(3, 1);
        assert(capacity.telemetry().generationCap == 2);
        capacity.observe(2, 1);
        capacity.observe(2, 1);
        assert(capacity.telemetry().generationCap == 1);
        assert(capacity.telemetry().singleFrameDuty == 1.0);

        capacity.observe(1, 1);
        assert(capacity.telemetry().singleFrameDuty < 1.0);
    }

    {
        // Rolling LSFG output reacts inside a sub-second window and requires
        // sustained evidence both to declare deficit and to prove recovery.
        LsfgOutputCadenceTracker cadence;
        cadence.configure(true, 60);
        for (int i = 0; i < 20; ++i)
            cadence.observe(33333333ns, 1, 0);
        assert(cadence.snapshot().valid);
        assert(cadence.snapshot().outputFps < 35.0);
        assert(cadence.snapshot().deficitConfirmed);
        assert(!cadence.snapshot().targetSatisfiedConfirmed);

        for (int i = 0; i < 36; ++i)
            cadence.observe(33333333ns, 1, 1);
        assert(cadence.snapshot().outputFps > 58.0);
        assert(!cadence.snapshot().deficitConfirmed);
        assert(cadence.snapshot().targetSatisfiedConfirmed);
    }

    {
        // Deadline admission predicts from observed GPU cost without source-rate
        // assumptions. The runtime may use this to reject synthetic work, while
        // the predictor itself remains independent of fractional scheduling.
        // The safety margin must turn an otherwise-fitting job into a rejection
        // when the remaining source-owned presentation budget is too small.
        DeadlineAdmissionPredictor predictor;
        const auto cold = predictor.predict(2, 12.0);
        assert(!cold.valid);

        predictor.observe(DeadlineAdmissionObservation{
            .mipmapsMs = 4.0,
            .opticalFlowMs = 6.0,
            .totalLsfgMs = 9.0,
            .generationCount = 2,
            .valid = true,
        });

        const auto roomy = predictor.predict(2, 12.0);
        assert(roomy.valid);
        assert(roomy.predictedMipmapsMs > 3.99 && roomy.predictedMipmapsMs < 4.01);
        assert(roomy.predictedOpticalFlowMs > 5.99 && roomy.predictedOpticalFlowMs < 6.01);
        assert(roomy.predictedTotalLsfgMs > 8.99 && roomy.predictedTotalLsfgMs < 9.01);
        assert(roomy.safetyMarginMs >= 0.35);
        assert(roomy.wouldAdmit);

        const auto tight = predictor.predict(2, 9.2);
        assert(tight.valid);
        assert(!tight.wouldAdmit);

        // Admission must also learn unmodeled end-to-end delivery pressure.
        // A frame that was predicted to fit but still arrived 2 ms late adds a
        // delivery reserve; repeated successful batches decay it instead of
        // turning one transient into a permanent throughput cap.
        const auto beforeMiss = predictor.predict(1, 8.5);
        assert(beforeMiss.valid);
        assert(beforeMiss.wouldAdmit);
        assert(beforeMiss.deliveryReserveMs == 0.0);

        predictor.observeDeliveryMiss(2.0);
        const auto afterMiss = predictor.predict(1, 8.5);
        assert(afterMiss.valid);
        assert(afterMiss.deliveryReserveMs >= 1.99);
        assert(afterMiss.effectiveUsableBudgetMs < beforeMiss.effectiveUsableBudgetMs);
        assert(!afterMiss.wouldAdmit);

        for (int i = 0; i < 100; ++i)
            predictor.observeDeliverySuccess();
        const auto recovered = predictor.predict(1, 8.5);
        assert(recovered.deliveryReserveMs < 0.05);
        assert(recovered.wouldAdmit);

        // The per-output residual scales with requested synthetic work while
        // mipmaps/flow remain shared costs. This is a prediction only; a later
        // rejected opportunity is never fed back into fractional scheduling.
        const auto oneOutput = predictor.predict(1, 12.0);
        assert(oneOutput.valid);
        assert(oneOutput.predictedTotalLsfgMs > 7.49);
        assert(oneOutput.predictedTotalLsfgMs < roomy.predictedTotalLsfgMs);

        const auto safeHint = predictor.safeGenerationHint(3, 33.0);
        assert(safeHint >= 1);
        assert(safeHint <= 3);
    }


    return 0;
}
