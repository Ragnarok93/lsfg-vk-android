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

        // Once cadence settles, the same safe hint may still promote one level
        // early rather than waiting the full generic demand interval.
        for (int frame = 0; frame < 8 && !promoted; ++frame) {
            scheduler.setSafeGenerationHint(2, true);
            scheduler.plan(40ms);
            promoted = promoted || scheduler.telemetry().capacityPromoted;
        }
        assert(promoted);
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

        bool sawRecoveryRaise = false;
        for (int i = 0; i < 24; ++i) {
            capacity.observe(2, 0);
            sawRecoveryRaise = sawRecoveryRaise
                || capacity.telemetry().raised;
        }
        assert(capacity.telemetry().generationCap == 3);
        assert(sawRecoveryRaise);
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
        // A WSI reduction is provisional. If the lower cap does not improve
        // useful delivery while the target remains recoverable, restore the
        // higher cap instead of turning presentation pressure into a target
        // governor.
        GeneratedPresentationCapacityTracker capacity;
        capacity.configure(3);
        GeneratedPresentationCapacityContext context{
            .outputDeficit = false,
            .deadlineCapacityValid = true,
            .safeGenerationHint = 3,
            .schedulerCostLimit = 3,
            .sourceInsideBudget = true,
            .higherCapacityProven = true,
        };

        for (int i = 0; i < 12; ++i) {
            const auto attempted = capacity.limit(3, context);
            const auto accepted = attempted > 0 ? attempted - 1 : 0;
            capacity.observe(attempted, accepted, attempted - accepted, context);
        }
        assert(capacity.telemetry().generationCap <= 2);

        context.outputDeficit = true;
        bool restored = false;
        for (int i = 0; i < 24; ++i) {
            const auto attempted = capacity.limit(3, context);
            capacity.observe(attempted, attempted, 0, context);
            restored = restored
                || capacity.telemetry().lastChangeReason
                    == GeneratedPresentationCapChangeReason::ProfitabilityRestoreHigher
                || capacity.telemetry().lastChangeReason
                    == GeneratedPresentationCapChangeReason::TargetDeficitProbeSuccess;
        }
        assert(restored);
        assert(capacity.telemetry().generationCap == 3);
    }

    {
        // Once the output target is deficient, a proven higher presentation
        // capacity must prevent persistent fractional-duty collapse. Hard WSI
        // pressure may still lower the integer cap, but it must not suppress
        // every remaining single-frame opportunity.
        GeneratedPresentationCapacityTracker capacity;
        capacity.configure(3);
        GeneratedPresentationCapacityContext collapse{};
        for (int i = 0; i < 80 && capacity.telemetry().generationCap > 1; ++i) {
            const auto attempted = capacity.limit(3, collapse);
            if (attempted > 0)
                capacity.observe(attempted, 0, attempted, collapse);
        }
        assert(capacity.telemetry().generationCap == 1);

        GeneratedPresentationCapacityContext recoverable{
            .outputDeficit = true,
            .deadlineCapacityValid = true,
            .safeGenerationHint = 3,
            .schedulerCostLimit = 3,
            .sourceInsideBudget = true,
            .higherCapacityProven = true,
        };
        for (int i = 0; i < 30; ++i) {
            const auto attempted = capacity.limit(3, recoverable);
            if (attempted > 0)
                capacity.observe(attempted, 0, attempted, recoverable);
        }
        assert(capacity.telemetry().singleFrameDuty >= 0.999);
    }

    {
        // A lower cap that preserves accepted throughput but does not materially
        // improve delivery efficiency is still unprofitable while the target is
        // unmet. It must be restored instead of becoming a hidden target cap.
        GeneratedPresentationCapacityTracker capacity;
        capacity.configure(8);
        GeneratedPresentationCapacityContext context{
            .outputDeficit = false,
            .deadlineCapacityValid = true,
            .safeGenerationHint = 8,
            .schedulerCostLimit = 8,
            .sourceInsideBudget = true,
            .higherCapacityProven = true,
        };

        for (int i = 0; i < 4; ++i) {
            const auto attempted = capacity.limit(8, context);
            capacity.observe(attempted, 4, attempted - 4, context);
        }
        assert(capacity.telemetry().generationCap == 7);

        context.outputDeficit = true;
        bool restored = false;
        for (int i = 0; i < 6; ++i) {
            const auto attempted = capacity.limit(8, context);
            capacity.observe(attempted, 4, attempted - 4, context);
            restored = restored
                || capacity.telemetry().lastChangeReason
                    == GeneratedPresentationCapChangeReason::ProfitabilityRestoreHigher;
        }
        assert(restored);
        assert(capacity.telemetry().generationCap == 8);
    }

    {
        // A target deficit must not preserve a higher presentation capacity
        // when the source timeline is already materially late. The source
        // evidence gate is what keeps an overdue real frame from being used to
        // justify more synthetic work.
        GeneratedPresentationCapacityTracker capacity;
        capacity.configure(1);
        GeneratedPresentationCapacityContext lateSource{
            .outputDeficit = true,
            .deadlineCapacityValid = true,
            .safeGenerationHint = 3,
            .schedulerCostLimit = 3,
            .sourceInsideBudget = true,
            .sourceDeadlineErrorNs = 9'000'000,
            .higherCapacityProven = true,
        };
        for (int i = 0; i < 40; ++i) {
            const auto attempted = capacity.limit(1, lateSource);
            if (attempted > 0)
                capacity.observe(attempted, 0, attempted, lateSource);
        }
        assert(capacity.telemetry().singleFrameDuty < 0.999);
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

        // Batch cost is learned independently by generation count. Unknown
        // counts use a conservative whole-batch bound; a measured 2-frame
        // batch must replace that fallback instead of being decomposed into a
        // misleading shared + linear per-frame model.
        const auto oneOutput = predictor.predict(1, 20.0);
        assert(oneOutput.valid);
        assert(oneOutput.predictedTotalLsfgMs >= roomy.predictedTotalLsfgMs * 0.5);

        predictor.observe(DeadlineAdmissionObservation{
            .mipmapsMs = 7.0,
            .opticalFlowMs = 11.0,
            .totalLsfgMs = 40.0,
            .generationCount = 2,
            .valid = true,
        });
        const auto measuredTwo = predictor.predict(2, 60.0);
        assert(measuredTwo.valid);
        assert(measuredTwo.predictedTotalLsfgMs >= 39.9);

        const auto conservativeThree = predictor.predict(3, 100.0);
        assert(conservativeThree.valid);
        assert(conservativeThree.predictedTotalLsfgMs >= 55.0);

        predictor.observe(DeadlineAdmissionObservation{
            .mipmapsMs = 8.0,
            .opticalFlowMs = 13.0,
            .totalLsfgMs = 58.0,
            .generationCount = 3,
            .valid = true,
        });
        const auto measuredThree = predictor.predict(3, 100.0);
        assert(measuredThree.valid);
        assert(measuredThree.predictedTotalLsfgMs >= 57.9);

        const auto safeHint = predictor.safeGenerationHint(3, 33.0);
        // With a measured 40 ms two-frame batch and a conservative lower-count
        // fallback, even the first evenly-spaced synthetic slot is not proven
        // safe inside a 33 ms source interval. Prefix-slot promotion must stay
        // closed rather than inventing capacity from the batch-only budget.
        assert(safeHint == 0);

        // Deferred Adreno protects the real-source boundary rather than
        // requiring compute to finish by the first ideal synthetic scanout.
        // The same measured batch can therefore be source-safe even when it
        // cannot satisfy the historical prefix-slot hint.
        DeadlineAdmissionPredictor deferredPredictor;
        deferredPredictor.observe(DeadlineAdmissionObservation{
            .mipmapsMs = 16.0,
            .opticalFlowMs = 18.0,
            .totalLsfgMs = 28.0,
            .generationCount = 1,
            .valid = true,
        });
        const auto slotHint =
            deferredPredictor.safeGenerationHint(1, 33.333);
        const auto batchHint =
            deferredPredictor.safeBatchGenerationHint(1, 33.333);
        assert(slotHint == 0);
        assert(batchHint == 1);

        const auto tooExpensiveBatchHint =
            deferredPredictor.safeBatchGenerationHint(1, 28.0);
        assert(tooExpensiveBatchHint == 0);

        // Protected Adreno blocks the source-present thread at private-device
        // completion. GPU shader timestamps alone can therefore understate the
        // real source-owned cost when queue residency expands under saturation.
        // Train admission from that measured blocking completion boundary.
        DeadlineAdmissionPredictor blockingPredictor;
        blockingPredictor.observe(DeadlineAdmissionObservation{
            .mipmapsMs = 12.0,
            .opticalFlowMs = 19.0,
            .totalLsfgMs = 24.0,
            .generationCount = 1,
            .valid = true,
        });
        const auto gpuOnlyDecision = blockingPredictor.predict(1, 37.0);
        assert(gpuOnlyDecision.valid);
        assert(gpuOnlyDecision.wouldAdmit);

        blockingPredictor.observeBlockingCompletion(1, 69.0);
        const auto blockingDecision = blockingPredictor.predict(1, 37.0);
        assert(blockingDecision.valid);
        assert(blockingDecision.predictedTotalLsfgMs > 68.9);
        assert(!blockingDecision.wouldAdmit);

        // Recovery from one saturated sample must be conservative so a single
        // fast frame cannot immediately reopen the same hitch-producing batch.
        blockingPredictor.observeBlockingCompletion(1, 24.0);
        const auto recoveryDecision = blockingPredictor.predict(1, 80.0);
        assert(recoveryDecision.predictedTotalLsfgMs > 60.0);

        // Once generated work is suppressed, no new completion samples exist.
        // Clean protected source-only cycles must therefore relax only the
        // queue-residency penalty toward the last measured GPU cost, never
        // below that cost, until a cautious probe can become admissible again.
        const auto stillBlocked = blockingPredictor.predict(1, 37.0);
        assert(!stillBlocked.wouldAdmit);

        // The GPU timestamp for the same completed batch is descriptive, not
        // recovery evidence. When it arrives after a 69 ms blocking completion,
        // it must not immediately dilute that source-owned cost back toward
        // the 24 ms shader time.
        DeadlineAdmissionPredictor sameBatchPredictor;
        sameBatchPredictor.observeBlockingCompletion(1, 69.0);
        sameBatchPredictor.observe(DeadlineAdmissionObservation{
            .mipmapsMs = 12.0,
            .opticalFlowMs = 19.0,
            .totalLsfgMs = 24.0,
            .generationCount = 1,
            .valid = true,
        });
        const auto sameBatchDecision = sameBatchPredictor.predict(1, 37.0);
        assert(sameBatchDecision.predictedTotalLsfgMs > 68.9);
        assert(!sameBatchDecision.wouldAdmit);
        for (int i = 0; i < 16; ++i)
            blockingPredictor.observeSourceOnlyRecovery();
        const auto recoveredProbe = blockingPredictor.predict(1, 37.0);
        assert(recoveredProbe.valid);
        assert(recoveredProbe.predictedTotalLsfgMs >= 23.9);
        assert(recoveredProbe.predictedTotalLsfgMs < 33.0);
        assert(recoveredProbe.wouldAdmit);
    }


    {
        // Fixed mode learns a source baseline without generated work, starts
        // conservatively, and reaches the requested ceiling only after stable
        // source cadence. No absolute FPS threshold participates.
        FixedSourceCadenceGovernor governor;
        assert(governor.plan(40ms, 3, 0, false) == 0);
        assert(governor.telemetry().baselineValid);

        std::size_t count = governor.plan(40ms, 3, 0, true);
        assert(count == 1);
        for (int i = 0; i < 12; ++i)
            count = governor.plan(40ms, 3, count, true);
        assert(count >= 2);
        for (int i = 0; i < 12; ++i)
            count = governor.plan(40ms, 3, count, true);
        assert(count == 3);
    }

    {
        // A severe cadence regression correlated with Fixed generated load must
        // back off before the slower cadence can inflate its own deadline budget.
        FixedSourceCadenceGovernor governor;
        governor.plan(40ms, 3, 0, false);
        std::size_t count = governor.plan(40ms, 3, 0, true);
        for (int i = 0; i < 20 && count < 3; ++i)
            count = governor.plan(40ms, 3, count, true);
        assert(count == 3);

        bool backedOff = false;
        for (int i = 0; i < 3; ++i) {
            count = governor.plan(70ms, 3, count, true);
            backedOff = backedOff || governor.telemetry().backedOff;
        }
        assert(backedOff);
        assert(count <= 2);
    }

    {
        // If even one synthetic frame causes a large source-cadence collapse,
        // Fixed mode may temporarily choose HistoryOnly rather than preserve the
        // multiplier at the expense of the real source timeline.
        FixedSourceCadenceGovernor governor;
        governor.plan(40ms, 1, 0, false);
        std::size_t count = governor.plan(40ms, 1, 0, true);
        assert(count == 1);
        bool backedOff = false;
        for (int i = 0; i < 3; ++i) {
            count = governor.plan(70ms, 1, count, true);
            backedOff = backedOff || governor.telemetry().backedOff;
        }
        assert(backedOff);
        assert(count == 0);

        // Only genuine source-only evidence may re-anchor a naturally slower
        // game cadence and eventually permit a cautious one-frame probe again.
        for (int i = 0; i < 20 && count == 0; ++i)
            count = governor.plan(
                60ms, 1, 0, true, SourceCadenceObservation::SourceOnly);
        assert(count == 1);
    }

    {
        // A HistoryOnly interval is still LSFG-active on protected Adreno.
        // Once Fixed backs off, those maintenance intervals must not redefine a
        // faster clean baseline as a naturally slower game and immediately
        // restart the same hitch-producing probe loop.
        FixedSourceCadenceGovernor governor;
        governor.plan(
            40ms, 1, 0, false, SourceCadenceObservation::SourceOnly);
        std::size_t count = governor.plan(
            40ms, 1, 0, true, SourceCadenceObservation::HistoryMaintenance);
        assert(count == 1);

        bool backedOff = false;
        for (int i = 0; i < 3; ++i) {
            count = governor.plan(
                70ms, 1, count, true, SourceCadenceObservation::Generated);
            backedOff = backedOff || governor.telemetry().backedOff;
        }
        assert(backedOff);
        assert(count == 0);

        for (int i = 0; i < 20; ++i)
            count = governor.plan(
                60ms, 1, 0, true,
                SourceCadenceObservation::HistoryMaintenance);
        assert(governor.telemetry().baselineSourceFps > 24.0);
        assert(count == 0);

        // Clean source-only evidence can still establish a genuinely slower
        // scene and eventually permit a cautious probe.
        for (int i = 0; i < 20 && count == 0; ++i)
            count = governor.plan(
                60ms, 1, 0, true, SourceCadenceObservation::SourceOnly);
        assert(count == 1);
    }

    {
        // Stable slow sources are treated identically to stable fast sources.
        FixedSourceCadenceGovernor governor;
        governor.plan(125ms, 2, 0, false);
        std::size_t count = governor.plan(125ms, 2, 0, true);
        assert(count == 1);
        for (int i = 0; i < 8; ++i)
            count = governor.plan(125ms, 2, count, true);
        assert(count == 2);
        assert(!governor.telemetry().backedOff);
    }




    {
        // Generation-first Adreno mode treats resource pressure as telemetry,
        // not permission to suppress requested synthetic work. A constrained
        // source must still be able to request more than one generated frame
        // when the adaptive target requires it.
        AdaptiveFrameScheduler scheduler(45, 3);
        scheduler.setGenerationFirst(true);

        std::size_t peak = 0;
        for (int i = 0; i < 12; ++i)
            peak = std::max(peak, scheduler.plan(70ms));

        assert(scheduler.telemetry().costLimit == 3);
        assert(peak >= 2);

        // A later severe source slowdown must not back the generation ceiling
        // down simply because the device is constrained.
        for (int i = 0; i < 8; ++i)
            scheduler.plan(120ms);
        assert(scheduler.telemetry().costLimit == 3);
    }


    // Generation-first Adreno executes one synthetic batch inside the real
    // source interval. A fixed 2x request must therefore not compare the
    // complete private-device batch against half of the source interval.
    assert(std::abs(framegenBatchBudgetMs(
        33.333, 16.666, true) - 33.333) < 0.001);
    assert(std::abs(framegenBatchBudgetMs(
        33.333, 16.666, false) - 16.666) < 0.001);

    return 0;
}
