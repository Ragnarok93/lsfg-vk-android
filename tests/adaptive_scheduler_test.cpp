#include "adaptive_scheduler.hpp"

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <string_view>

using namespace std::chrono_literals;

int main() {
    // Disabled schedulers never generate work.
    {
        AdaptiveFrameScheduler scheduler;
        for (int frame = 0; frame < 12; ++frame)
            assert(scheduler.plan(33ms) == 0);
        const auto diagnostics = scheduler.diagnostics();
        assert(diagnostics.targetFps == 0);
        assert(diagnostics.generationCeiling == 0);
        assert(!diagnostics.probing);
    }

    // A newly enabled controller establishes an unmodified source-FPS baseline
    // before it starts probing generation levels.
    {
        AdaptiveFrameScheduler scheduler(60, 3);
        for (int frame = 0; frame < 6; ++frame)
            assert(scheduler.plan(20ms) == 0);
        assert(scheduler.generationCeiling() == 1);
        const auto diagnostics = scheduler.diagnostics();
        assert(diagnostics.targetFps == 60);
        assert(std::abs(diagnostics.smoothedSourceFps - 50.0) < 0.5);
        assert(diagnostics.probing);
    }

    // Fractional ratios remain fractional: 50 FPS -> 60 FPS should average
    // roughly one generated frame per five source frames, not force 2x.
    {
        AdaptiveFrameScheduler scheduler(60, 3);
        for (int frame = 0; frame < 6; ++frame)
            assert(scheduler.plan(20ms) == 0);

        std::size_t generated = 0;
        for (int frame = 0; frame < 50; ++frame)
            generated += scheduler.plan(20ms);
        assert(generated >= 9 && generated <= 11);
        assert(scheduler.generationCeiling() == 1);

        const auto diagnostics = scheduler.diagnostics();
        assert(std::abs(diagnostics.smoothedSourceFps - 50.0) < 0.5);
        assert(std::abs(diagnostics.requestedGeneratedFrames - 0.2) < 0.05);
        assert(diagnostics.generationCeiling == 1);
        assert(diagnostics.provenGenerationCeiling == 1);
        assert(diagnostics.cooldownSamples == 0);
        assert(!diagnostics.probing);
    }

    // The adaptive target is an objective, not a source-frame limiter. A game
    // already above target simply receives no generated frames.
    {
        AdaptiveFrameScheduler scheduler(60, 3);
        for (int frame = 0; frame < 40; ++frame)
            assert(scheduler.plan(10ms) == 0);
        const auto diagnostics = scheduler.diagnostics();
        assert(diagnostics.requestedGeneratedFrames == 0.0);
        assert(std::abs(diagnostics.smoothedSourceFps - 100.0) < 0.5);
    }

    // A sustained deficit that genuinely benefits from additional generation
    // is allowed to climb through successively proven levels.
    {
        AdaptiveFrameScheduler scheduler(120, 3);
        bool sawThreeGenerated = false;
        for (int frame = 0; frame < 180; ++frame) {
            const auto generated = scheduler.plan(33333333ns);
            sawThreeGenerated = sawThreeGenerated || generated == 3;
        }
        assert(scheduler.provenGenerationCeiling() >= 2);
        assert(scheduler.generationCeiling() == 3);
        assert(sawThreeGenerated);
        const auto diagnostics = scheduler.diagnostics();
        assert(diagnostics.targetFps == 120);
        assert(diagnostics.requestedGeneratedFrames > 2.9);
    }

    // If a higher FG level collapses real source cadence, reject the probe,
    // return to the proven level, and do not immediately probe it again.
    {
        AdaptiveFrameScheduler scheduler(120, 3);
        std::size_t previousGenerated = 0;
        bool reachedThree = false;
        bool rejectedThree = false;

        for (int frame = 0; frame < 360; ++frame) {
            std::chrono::nanoseconds interval = 33333333ns; // ~30 real FPS baseline
            if (previousGenerated == 1)
                interval = 34ms;
            else if (previousGenerated == 2)
                interval = 40ms;
            else if (previousGenerated == 3)
                interval = 60ms; // source collapse at the probe level

            previousGenerated = scheduler.plan(interval);
            reachedThree = reachedThree || previousGenerated == 3;
            const auto diagnostics = scheduler.diagnostics();
            if (reachedThree && diagnostics.lastProbeRejected) {
                rejectedThree = true;
                assert(diagnostics.generationCeiling == 2);
                assert(diagnostics.provenGenerationCeiling == 2);
                assert(diagnostics.cooldownSamples > 0);
                assert(!diagnostics.probing);
                assert(std::string_view(diagnostics.lastDecisionReason) == "source-collapse"
                    || std::string_view(diagnostics.lastDecisionReason)
                        == "throughput-and-source-regression");
                break;
            }
        }

        assert(reachedThree);
        assert(rejectedThree);
        assert(scheduler.provenGenerationCeiling() == 2);

        for (int frame = 0; frame < 30; ++frame) {
            const auto generated = scheduler.plan(40ms);
            assert(generated <= 2);
            assert(scheduler.generationCeiling() <= 2);
        }
    }

    // Measured FG cost can reject a probe even while pure game cadence remains
    // stable. This is the cross-GPU contract: an A6xx or Xclipse path backs off
    // according to relative budget pressure rather than a vendor-specific ms cap.
    {
        AdaptiveFrameScheduler scheduler(120, 3);
        std::size_t previousGenerated = 0;
        bool rejectedForCost = false;
        for (int frame = 0; frame < 160; ++frame) {
            AdaptiveFrameScheduler::StageCosts costs{};
            if (previousGenerated > 0)
                costs.generatedPresentMs = 15.0; // 75% of a 20 ms source budget
            previousGenerated = scheduler.plan(20ms, costs);
            const auto diagnostics = scheduler.diagnostics();
            if (diagnostics.lastProbeRejected) {
                rejectedForCost = true;
                assert(std::string_view(diagnostics.lastDecisionReason)
                    == "stage-cost-pressure");
                assert(diagnostics.lastStageCostRatio > 0.70);
                break;
            }
        }
        assert(rejectedForCost);
        assert(scheduler.provenGenerationCeiling() == 0);
        assert(scheduler.generationCeiling() == 0);
    }

    // Fractional carry must remain accurate over a long run. 40 -> 70 FPS asks
    // for 0.75 generated frames per source frame; bounded carry may not create a
    // systematic undershoot of the requested long-run average.
    {
        AdaptiveFrameScheduler scheduler(70, 2);
        for (int frame = 0; frame < 6; ++frame)
            assert(scheduler.plan(25ms) == 0);
        std::size_t generated = 0;
        constexpr int samples = 400;
        for (int frame = 0; frame < samples; ++frame)
            generated += scheduler.plan(25ms);
        const double generatedPerSource = static_cast<double>(generated) / samples;
        assert(std::abs(generatedPerSource - 0.75) < 0.03);
    }

    // If the path executes a source-history warmup instead of the planned FG
    // count, the next sample is attributed to the actual zero-generation level.
    {
        AdaptiveFrameScheduler scheduler(120, 2);
        for (int frame = 0; frame < 6; ++frame)
            scheduler.plan(20ms);
        const auto planned = scheduler.plan(20ms);
        assert(planned > 0);
        scheduler.noteActualGenerationCount(0);
        assert(scheduler.diagnostics().lastGeneratedFrameCount == 0);
    }

    // App switches / shader stalls invalidate timing history and force a fresh
    // source-only baseline instead of producing a catch-up burst.
    {
        AdaptiveFrameScheduler scheduler(120, 3);
        for (int frame = 0; frame < 80; ++frame)
            scheduler.plan(33333333ns);
        assert(scheduler.plan(1s) == 0);
        auto diagnostics = scheduler.diagnostics();
        assert(diagnostics.smoothedSourceFps == 0.0);
        assert(diagnostics.generationCeiling == 0);
        assert(diagnostics.provenGenerationCeiling == 0);
        for (int frame = 0; frame < 6; ++frame)
            assert(scheduler.plan(33333333ns) == 0);
    }

    // Hot changes do not inherit fractional/probe state from the old target.
    {
        AdaptiveFrameScheduler scheduler(60, 3);
        for (int frame = 0; frame < 30; ++frame)
            scheduler.plan(20ms);
        scheduler.configure(90, 3);
        assert(scheduler.targetFps() == 90);
        assert(scheduler.provenGenerationCeiling() == 0);
        auto diagnostics = scheduler.diagnostics();
        assert(diagnostics.requestedGeneratedFrames == 0.0);
        assert(diagnostics.cooldownSamples == 0);
        assert(!diagnostics.probing);
        for (int frame = 0; frame < 6; ++frame)
            assert(scheduler.plan(33333333ns) == 0);
    }

    return 0;
}
