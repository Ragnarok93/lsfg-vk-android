#include "adaptive_flow_controller.hpp"

#include <cassert>
#include <chrono>
#include <cmath>

using namespace std::chrono_literals;

namespace {

AdaptiveFlowObservation sample(
        double totalMs,
        double flowMs,
        double budgetMs = 16.666,
        bool schedulerTransition = false,
        bool deadlineMissed = false,
        double globalGpuUsagePercent = 0.0,
        bool globalPressureValid = false,
        bool outputDeficit = false,
        bool syntheticDropPressure = false,
        bool generatedWorkSample = true) {
    return AdaptiveFlowObservation{
        .elapsed = 100ms,
        .frameBudgetMs = budgetMs,
        .totalLsfgMs = totalMs,
        .flowMs = flowMs,
        .mipmapsMs = flowMs * 0.4,
        .generationCount = 1,
        .deadlineMissed = deadlineMissed,
        .globalGpuUsagePercent = globalGpuUsagePercent,
        .globalPressureValid = globalPressureValid,
        .outputDeficit = outputDeficit,
        .syntheticDropPressure = syntheticDropPressure,
        .generatedWorkSample = generatedWorkSample,
        .schedulerTransition = schedulerTransition,
        .valid = true,
    };
}

bool near(float a, float b) {
    return std::fabs(a - b) < 0.0001F;
}

} // namespace

int main() {
    {
        const auto autoStates = AdaptiveFlowController::statesForPreset(AdaptiveFlowPreset::Auto);
        constexpr float expected[] = {
            1.00F, 0.95F, 0.90F, 0.85F, 0.80F, 0.75F, 0.70F, 0.65F,
            0.60F, 0.55F, 0.50F, 0.45F, 0.40F, 0.35F, 0.30F, 0.25F,
        };
        assert(autoStates.size() == 16);
        for (std::size_t i = 0; i < autoStates.size(); ++i) {
            assert(near(autoStates[i], expected[i]));
            if (i > 0)
                assert(near(autoStates[i - 1] - autoStates[i], 0.05F));
        }

        AdaptiveFlowController controller(AdaptiveFlowPreset::Auto);
        assert(controller.telemetry().stateCount == autoStates.size());
        assert(near(controller.telemetry().targetScale, 1.00F));
        assert(near(controller.telemetry().minimumScale, 0.25F));
        assert(controller.telemetry().stateIndex == 0);
    }

    {
        const auto quality = AdaptiveFlowController::statesForPreset(AdaptiveFlowPreset::Quality);
        const auto balanced = AdaptiveFlowController::statesForPreset(AdaptiveFlowPreset::Balanced);
        const auto low = AdaptiveFlowController::statesForPreset(AdaptiveFlowPreset::Low);
        assert(quality.size() == 7 && near(quality.front(), 1.00F) && near(quality.back(), 0.70F));
        assert(balanced.size() == 6 && near(balanced.front(), 0.80F) && near(balanced.back(), 0.55F));
        assert(low.size() == 7 && near(low.front(), 0.55F) && near(low.back(), 0.25F));
    }

    {
        // All adaptive presets use exact .05 state spacing while preserving
        // their established target and floor envelopes.
        const auto quality = AdaptiveFlowController::statesForPreset(AdaptiveFlowPreset::Quality);
        const auto balanced = AdaptiveFlowController::statesForPreset(AdaptiveFlowPreset::Balanced);
        const auto low = AdaptiveFlowController::statesForPreset(AdaptiveFlowPreset::Low);
        constexpr float qualityExpected[] = {
            1.00F, 0.95F, 0.90F, 0.85F, 0.80F, 0.75F, 0.70F,
        };
        constexpr float balancedExpected[] = {
            0.80F, 0.75F, 0.70F, 0.65F, 0.60F, 0.55F,
        };
        constexpr float lowExpected[] = {
            0.55F, 0.50F, 0.45F, 0.40F, 0.35F, 0.30F, 0.25F,
        };
        const auto verifyStates = [](std::span<const float> actual,
                const float* expected, std::size_t expectedCount) {
            assert(actual.size() == expectedCount);
            for (std::size_t i = 0; i < expectedCount; ++i) {
                assert(near(actual[i], expected[i]));
                if (i > 0)
                    assert(near(actual[i - 1] - actual[i], 0.05F));
            }
        };
        verifyStates(quality, qualityExpected, 7);
        verifyStates(balanced, balancedExpected, 6);
        verifyStates(low, lowExpected, 7);
    }

    {
        // Direct target pressure reacts quickly, but advances only one .05
        // state per decision so every Flow state is measured before another
        // graph handoff is requested.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Auto);
        bool changed = false;
        for (int i = 0; i < 16 && !changed; ++i) {
            auto observation = sample(
                36.3, 11.7, 66.666, false, false,
                97.0, true, true, false, true);
            observation.outputTargeted = true;
            observation.outputCadenceValid = true;
            observation.outputFps = 49.8;
            observation.outputTargetSatisfied = false;
            observation.sourceFps = 12.4;
            controller.observe(observation);
            changed = controller.telemetry().changed;
        }
        assert(changed);
        assert(near(controller.currentScale(), 0.95F));
        assert(controller.telemetry().stateIndex == 1);
    }

    {
        // Cold Adaptive Flow contexts must start from the persisted fixed Flow
        // scale (or the closest no-more-expensive preset state), not blindly
        // jump to the preset ceiling. This preserves a known-good pacing state
        // across app relaunches as well as runtime mode toggles.
        const auto autoSeed =
            AdaptiveFlowController::conservativeSeedScale(
                AdaptiveFlowPreset::Auto, 0.52F);
        assert(autoSeed.has_value() && near(*autoSeed, 0.50F));

        const auto qualityBelowFloor =
            AdaptiveFlowController::conservativeSeedScale(
                AdaptiveFlowPreset::Quality, 0.50F);
        assert(qualityBelowFloor.has_value()
            && near(*qualityBelowFloor, 0.70F));

        const auto balancedBetweenStates =
            AdaptiveFlowController::conservativeSeedScale(
                AdaptiveFlowPreset::Balanced, 0.78F);
        assert(balancedBetweenStates.has_value()
            && near(*balancedBetweenStates, 0.75F));

        const auto lowAboveTarget =
            AdaptiveFlowController::conservativeSeedScale(
                AdaptiveFlowPreset::Low, 0.80F);
        assert(lowAboveTarget.has_value() && near(*lowAboveTarget, 0.55F));

        const auto invalid =
            AdaptiveFlowController::conservativeSeedScale(
                AdaptiveFlowPreset::Auto,
                std::numeric_limits<float>::quiet_NaN());
        assert(!invalid.has_value());
    }

    {
        // Adaptive always starts at the preset target and still rejects a
        // sub-confirmation pressure burst after the faster control-loop tune.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Quality);
        assert(near(controller.currentScale(), 1.00F));
        for (int i = 0; i < 3; ++i)
            controller.observe(sample(16.0, 5.0));
        assert(near(controller.currentScale(), 1.00F));
    }

    {
        // A direct missed output target is user-visible pressure. Confirmation
        // is fast, but a decision may move only one discrete Flow state.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Quality);
        bool changed = false;
        for (int i = 0; i < 6 && !changed; ++i) {
            auto observation = sample(16.0, 5.0, 50.0);
            observation.outputCadenceValid = true;
            observation.outputTargeted = true;
            observation.outputTargetSatisfied = false;
            observation.outputDeficit = true;
            observation.outputFps = 42.0;
            observation.sourceFps = 16.0;
            controller.observe(observation);
            changed = controller.telemetry().changed;
        }
        assert(changed);
        assert(near(controller.currentScale(), 0.95F));
        assert(controller.telemetry().stateIndex == 1);
    }

    {
        // Recovery is also one .05 state per confirmed decision. This prevents
        // a quality upstep from skipping over a state whose cost has not yet
        // been measured.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Quality);
        bool lowered = false;
        for (int i = 0; i < 8 && !lowered; ++i) {
            auto observation = sample(16.0, 5.0, 50.0);
            observation.outputCadenceValid = true;
            observation.outputTargeted = true;
            observation.outputTargetSatisfied = false;
            observation.outputDeficit = true;
            observation.outputFps = 42.0;
            observation.sourceFps = 16.0;
            controller.observe(observation);
            lowered = near(controller.currentScale(), 0.95F);
        }
        assert(lowered);

        auto transition = sample(12.0, 3.0, 50.0);
        transition.flowTransition = true;
        controller.observe(transition);
        for (int i = 0; i < 3; ++i)
            controller.observe(sample(12.0, 3.0, 50.0));

        bool benefitConfirmed = false;
        for (int i = 0; i < 6 && !benefitConfirmed; ++i) {
            auto observation = sample(12.0, 3.0, 50.0);
            observation.outputCadenceValid = true;
            observation.outputTargeted = true;
            observation.outputTargetSatisfied = false;
            observation.outputDeficit = true;
            observation.outputFps = 44.0;
            observation.sourceFps = 18.0;
            controller.observe(observation);
            benefitConfirmed = controller.telemetry().reason
                == AdaptiveFlowDecisionReason::DownstepBenefitConfirmed;
        }
        assert(benefitConfirmed);
        assert(near(controller.currentScale(), 0.95F));

        bool recovered = false;
        for (int i = 0; i < 40 && !recovered; ++i) {
            auto observation = sample(8.0, 2.0, 50.0);
            observation.outputCadenceValid = true;
            observation.outputTargeted = true;
            observation.outputTargetSatisfied = true;
            observation.outputDeficit = false;
            observation.outputFps = 65.0;
            observation.sourceFps = 30.0;
            controller.observe(observation);
            recovered = controller.telemetry().reason
                == AdaptiveFlowDecisionReason::SustainedHeadroom;
        }
        assert(recovered);
        assert(near(controller.currentScale(), 1.00F));
        assert(controller.telemetry().stateIndex == 0);
    }

    {
        // Adaptive-FG quality recovery is a probe, not permission to sacrifice
        // source cadence. If one higher Flow state turns a stable 30+30 -> 60
        // regime into ~20 source + ~40 generated -> 60, revert immediately:
        // average output FPS alone does not make that presentation cadence safe.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Auto);
        assert(controller.seedCurrentScale(0.50F));

        auto healthy = sample(8.0, 2.0, 33.333);
        healthy.adaptiveFramegenMode = true;
        healthy.scheduledGenerationDensity = 1.0;
        healthy.sourceFps = 30.0;
        healthy.outputCadenceValid = true;
        healthy.outputTargeted = true;
        healthy.outputTargetSatisfied = true;
        healthy.outputFps = 60.0;

        bool raised = false;
        for (int i = 0; i < 30 && !raised; ++i) {
            controller.observe(healthy);
            raised = near(controller.currentScale(), 0.55F);
        }
        assert(raised);

        auto transition = healthy;
        transition.flowTransition = true;
        controller.observe(transition);
        for (int i = 0; i < 4; ++i)
            controller.observe(healthy);

        bool reverted = false;
        for (int i = 0; i < 12 && !reverted; ++i) {
            auto degraded = sample(14.0, 3.0, 50.0);
            degraded.adaptiveFramegenMode = true;
            degraded.scheduledGenerationDensity = 2.0;
            degraded.sourceFps = 20.0;
            degraded.outputCadenceValid = true;
            degraded.outputTargeted = true;
            degraded.outputTargetSatisfied = true;
            degraded.outputFps = 60.0;
            controller.observe(degraded);
            reverted = controller.telemetry().reason
                == AdaptiveFlowDecisionReason::RecoveryPacingReverted;
        }

        assert(reverted);
        assert(near(controller.currentScale(), 0.50F));

        // Do not immediately retry the same damaging quality probe. Repeated
        // 0.50 -> 0.55 -> 0.50 cycles would themselves become a periodic
        // pacing fault even though each individual rollback is correct.
        for (int i = 0; i < 100; ++i)
            controller.observe(healthy);
        assert(near(controller.currentScale(), 0.50F));

        // The rejected quality state's retry hold must not suppress emergency
        // downscaling. If real pressure appears, Flow still needs to move lower.
        bool pressureLowered = false;
        for (int i = 0; i < 8 && !pressureLowered; ++i) {
            auto pressure = sample(34.0, 10.0, 33.333);
            pressure.adaptiveFramegenMode = true;
            pressure.scheduledGenerationDensity = 1.0;
            pressure.sourceFps = 30.0;
            pressure.outputCadenceValid = true;
            pressure.outputTargeted = true;
            pressure.outputTargetSatisfied = false;
            pressure.outputDeficit = true;
            pressure.outputFps = 50.0;
            controller.observe(pressure);
            pressureLowered = controller.currentScale() < 0.50F;
        }
        assert(pressureLowered);
    }

    {
        // Regression: on generation-first Adreno, Adaptive FG can satisfy the
        // 60 FPS output target by increasing synthetic density after Flow work
        // has already damaged the real/source cadence. Output satisfaction must
        // not hide that source-health regression. Adaptive Flow should probe one
        // cheaper state and retain it only when source cadence improves.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Auto);
        assert(controller.seedCurrentScale(0.45F));

        auto healthy = sample(10.0, 3.0, 33.333);
        healthy.adaptiveFramegenMode = true;
        healthy.scheduledGenerationDensity = 1.0;
        healthy.sourceFps = 30.0;
        healthy.outputCadenceValid = true;
        healthy.outputTargeted = true;
        healthy.outputTargetSatisfied = true;
        healthy.outputDeficit = false;
        healthy.outputFps = 60.0;
        healthy.globalGpuUsagePercent = 82.0;
        healthy.globalPressureValid = true;
        for (int i = 0; i < 12; ++i)
            controller.observe(healthy);
        assert(near(controller.currentScale(), 0.45F));

        bool sourceProtected = false;
        for (int i = 0; i < 10 && !sourceProtected; ++i) {
            auto feedbackLoop = sample(14.0, 4.0, 55.0);
            feedbackLoop.adaptiveFramegenMode = true;
            feedbackLoop.scheduledGenerationDensity = 2.30;
            feedbackLoop.sourceFps = 18.0;
            feedbackLoop.sourceBlockingRatio = 0.90;
            feedbackLoop.sourceBlockingPressure = true;
            feedbackLoop.outputCadenceValid = true;
            feedbackLoop.outputTargeted = true;
            feedbackLoop.outputTargetSatisfied = true;
            feedbackLoop.outputDeficit = false;
            feedbackLoop.outputFps = 60.0;
            feedbackLoop.globalGpuUsagePercent = 96.0;
            feedbackLoop.globalPressureValid = true;
            controller.observe(feedbackLoop);
            sourceProtected = controller.currentScale() < 0.45F;
        }
        assert(sourceProtected);
        assert(near(controller.currentScale(), 0.40F));

        // A source slowdown with negligible scale-sensitive Flow work is not a
        // reason to trade quality. The source-health path is an actuator probe,
        // not a generic reaction to CPU-bound game slowdown.
        AdaptiveFlowController cpuBound(AdaptiveFlowPreset::Auto);
        assert(cpuBound.seedCurrentScale(0.45F));
        for (int i = 0; i < 12; ++i)
            cpuBound.observe(healthy);
        for (int i = 0; i < 20; ++i) {
            auto slowdown = sample(5.0, 0.10, 55.0);
            slowdown.adaptiveFramegenMode = true;
            slowdown.scheduledGenerationDensity = 2.30;
            slowdown.sourceFps = 18.0;
            slowdown.sourceBlockingRatio = 0.12;
            slowdown.sourceBlockingPressure = false;
            slowdown.outputCadenceValid = true;
            slowdown.outputTargeted = true;
            slowdown.outputTargetSatisfied = true;
            slowdown.outputDeficit = false;
            slowdown.outputFps = 60.0;
            slowdown.globalGpuUsagePercent = 55.0;
            slowdown.globalPressureValid = true;
            cpuBound.observe(slowdown);
        }
        assert(cpuBound.currentScale() >= 0.45F);
    }

    {
        // Sustained pressure with a material scale-sensitive contribution lowers
        // one state, then observes a cooldown instead of cascading immediately.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Quality);
        for (int i = 0; i < 5; ++i)
            controller.observe(sample(16.2, 5.0));
        assert(near(controller.currentScale(), 0.95F));
        assert(controller.telemetry().reason == AdaptiveFlowDecisionReason::SustainedPressure
            || controller.telemetry().reason == AdaptiveFlowDecisionReason::EvaluatingDownstep
            || controller.telemetry().reason == AdaptiveFlowDecisionReason::Cooldown);
        bool benefitConfirmed = false;
        for (int i = 0; i < 10; ++i) {
            controller.observe(sample(13.0, 3.5));
            benefitConfirmed = benefitConfirmed
                || controller.telemetry().reason
                    == AdaptiveFlowDecisionReason::DownstepBenefitConfirmed;
        }
        assert(benefitConfirmed);
        assert(near(controller.currentScale(), 0.95F));
    }

    {
        // High total LSFG pressure is not enough when Flow Scale cannot
        // materially change the budget.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Balanced);
        for (int i = 0; i < 30; ++i)
            controller.observe(sample(16.3, 0.45));
        assert(near(controller.currentScale(), 0.80F));
    }

    {
        // Adaptive-LSFG transitions suppress evidence so both governors do not
        // react to the same transient.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Balanced);
        for (int i = 0; i < 12; ++i)
            controller.observe(sample(16.2, 5.0));
        auto transition = sample(16.2, 5.0, 16.666, true);
        transition.flowTransition = true;
        controller.observe(transition);
        bool sawTransitionSettle = false;
        for (int i = 0; i < 40; ++i) {
            controller.observe(sample(16.2, 5.0));
            sawTransitionSettle = sawTransitionSettle
                || controller.telemetry().reason
                    == AdaptiveFlowDecisionReason::FlowTransitionSettle;
        }
        assert(sawTransitionSettle);
        // The transition barrier spans the entire post-handoff dwell. A second
        // downstep may happen only after that dwell and a fresh pressure window.
        assert(near(controller.currentScale(), 0.80F));
        bool sawSecondDownstep = false;
        for (int i = 0; i < 80 && !sawSecondDownstep; ++i) {
            controller.observe(sample(16.2, 5.0));
            sawSecondDownstep = controller.telemetry().reason
                == AdaptiveFlowDecisionReason::SustainedPressure;
        }
        assert(sawSecondDownstep);
        for (int i = 0; i < 10; ++i)
            controller.observe(sample(13.0, 3.5));
        assert(near(controller.currentScale(), 0.75F));
    }

    {
        // Degradation never passes the preset hard floor, but each downstep
        // must first prove useful before another downstep is allowed.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Low);
        for (int step = 0; step < 6; ++step) {
            const float before = controller.currentScale();
            for (int i = 0; i < 30 && near(controller.currentScale(), before); ++i)
                controller.observe(sample(16.4, 7.0, 16.666, false, true));
            assert(!near(controller.currentScale(), before));
            for (int i = 0; i < 10; ++i)
                controller.observe(sample(12.0, 4.0));
        }
        assert(near(controller.currentScale(), 0.25F));
        for (int i = 0; i < 40; ++i)
            controller.observe(sample(16.4, 7.0, 16.666, false, true));
        assert(near(controller.currentScale(), 0.25F));
    }

    {
        // Recovery remains slower than downscaling, but the faster controller
        // must restore quality inside roughly half the previous dwell.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Quality);
        for (int i = 0; i < 5; ++i)
            controller.observe(sample(16.2, 5.0));
        assert(near(controller.currentScale(), 0.95F));
        for (int i = 0; i < 10; ++i)
            controller.observe(sample(8.0, 2.0));
        assert(near(controller.currentScale(), 0.95F));
        for (int i = 0; i < 12; ++i)
            controller.observe(sample(8.0, 2.0));
        assert(near(controller.currentScale(), 1.00F));
    }

    {
        // Recovery should be quality-seeking: once the higher state is
        // predicted to fit comfortably, the lower state's raw utilization
        // ratio must not strand quality indefinitely.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Quality);
        for (int i = 0; i < 5; ++i)
            controller.observe(sample(16.2, 5.0));
        assert(near(controller.currentScale(), 0.95F));

        for (int i = 0; i < 70; ++i)
            controller.observe(sample(12.5, 1.0));
        assert(near(controller.currentScale(), 1.00F));
    }

    {
        // If a higher state is predicted to consume too much of the budget,
        // hold the lower state despite otherwise healthy current timings.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Quality);
        for (int i = 0; i < 5; ++i)
            controller.observe(sample(16.2, 6.0));
        assert(near(controller.currentScale(), 0.95F));
        for (int i = 0; i < 60; ++i)
            controller.observe(sample(14.0, 9.0));
        assert(near(controller.currentScale(), 0.95F));
    }

    {
        // A suspend-sized observation must not satisfy several seconds of
        // recovery evidence in one call. This matters for Adaptive Flow paired
        // with Fixed LSFG, where no Adaptive-LSFG discontinuity event exists.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Quality);
        for (int i = 0; i < 5; ++i)
            controller.observe(sample(16.2, 5.0));
        assert(near(controller.currentScale(), 0.95F));

        for (int i = 0; i < 10; ++i)
            controller.observe(sample(8.0, 2.0));
        auto resumed = sample(8.0, 2.0);
        resumed.elapsed = 10s;
        controller.observe(resumed);
        assert(near(controller.currentScale(), 0.95F));
    }

    {
        // Whole-device GPU saturation plus a real output deficit must lower
        // Flow Scale even when the current cycle is history-only. The retained
        // generated-work timing supplies the scale-sensitive relief estimate.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Quality);
        for (int i = 0; i < 3; ++i) {
            controller.observe(sample(
                8.0, 5.0, 16.666, false, false,
                99.0, true, true, false, false));
        }
        assert(near(controller.currentScale(), 0.95F));
        assert(
            controller.telemetry().reason
                == AdaptiveFlowDecisionReason::SustainedGlobalPressure
            || controller.telemetry().reason
                == AdaptiveFlowDecisionReason::EvaluatingDownstep
            || controller.telemetry().reason
                == AdaptiveFlowDecisionReason::Cooldown);
    }

    {
        // History-only operation may recover quality only from retained timing
        // of a real generated cycle plus fresh whole-device headroom and an
        // already-met LSFG output target. This prevents a cheap zero-generation
        // cycle from pretending generation is cheap while avoiding permanent
        // quality pinning once Adaptive no longer needs synthetic frames.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Quality);
        for (int i = 0; i < 3; ++i) {
            controller.observe(sample(
                8.0, 5.0, 16.666, false, false,
                99.0, true, true, false, false));
        }
        assert(near(controller.currentScale(), 0.95F));

        for (int i = 0; i < 70; ++i) {
            controller.observe(sample(
                5.0, 1.5, 16.666, false, false,
                45.0, true, false, false, false));
        }
        assert(near(controller.currentScale(), 1.00F));
    }

    {
        // Retained history timing alone is insufficient when current global
        // headroom is unavailable. Do not upscale from stale timing blindly.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Quality);
        for (int i = 0; i < 3; ++i) {
            controller.observe(sample(
                8.0, 5.0, 16.666, false, false,
                99.0, true, true, false, false));
        }
        assert(near(controller.currentScale(), 0.95F));

        // Establish that the downstep itself was useful before testing whether
        // stale headroom is allowed to restore quality.
        for (int i = 0; i < 10; ++i) {
            controller.observe(sample(
                5.5, 1.8, 16.666, false, false,
                90.0, true, false, false, false));
        }
        assert(near(controller.currentScale(), 0.95F));

        for (int i = 0; i < 70; ++i) {
            controller.observe(sample(
                5.0, 1.5, 16.666, false, false,
                0.0, false, false, false, false));
        }
        assert(near(controller.currentScale(), 0.95F));
    }

    {
        // WSI pressure belongs to the presentation-capacity governor unless
        // global GPU pressure also makes Flow a plausible actuator.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Quality);
        for (int i = 0; i < 30; ++i) {
            auto observation = sample(
                8.0, 3.0, 16.666, false, false,
                82.0, true, true, false, false);
            observation.wsiPresentationPressure = true;
            observation.wsiLossRate = 0.35;
            observation.outputTargeted = true;
            controller.observe(observation);
        }
        assert(near(controller.currentScale(), 1.00F));
    }

    {
        // A missing timing sample must not leave the previous pressure values
        // visible as if they described the current cycle.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Quality);
        controller.observe(sample(16.0, 18.0, 12.0, true, true));
        assert(controller.telemetry().pressureRatio > 1.0);

        auto invalid = sample(0.0, 0.0, 0.0);
        invalid.valid = false;
        controller.observe(invalid);
        assert(near(controller.telemetry().pressureRatio, 0.0));
        assert(near(controller.telemetry().flowBudgetRatio, 0.0));
        assert(!controller.telemetry().computePressure);
        assert(!controller.telemetry().wsiPressure);
    }

    {
        // A WSI/global-pressure downstep is provisional. No measurable benefit
        // restores the previous quality state and applies a longer hold.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Quality);
        bool lowered = false;
        for (int i = 0; i < 12 && !lowered; ++i) {
            auto observation = sample(
                8.0, 5.0, 16.666, false, false,
                99.0, true, true, false, false);
            observation.wsiPresentationPressure = true;
            observation.wsiLossRate = 0.35;
            observation.outputTargeted = true;
            observation.outputCadenceValid = true;
            observation.outputFps = 54.0;
            observation.sourceFps = 30.0;
            controller.observe(observation);
            lowered = near(controller.currentScale(), 0.95F);
        }
        assert(lowered);

        bool reverted = false;
        for (int i = 0; i < 12; ++i) {
            auto observation = sample(
                8.0, 5.0, 16.666, false, false,
                99.0, true, true, false, false);
            observation.wsiPresentationPressure = true;
            observation.wsiLossRate = 0.35;
            observation.outputTargeted = true;
            observation.outputCadenceValid = true;
            observation.outputFps = 54.0;
            observation.sourceFps = 30.0;
            controller.observe(observation);
            reverted = reverted
                || controller.telemetry().reason
                    == AdaptiveFlowDecisionReason::DownstepReverted;
        }
        assert(reverted);
        assert(near(controller.currentScale(), 1.00F));
        for (int i = 0; i < 20; ++i)
            controller.observe(sample(16.2, 5.0));
        assert(near(controller.currentScale(), 1.00F));
    }

    {
        // A compute-limited downstep that relieves LSFG pressure is retained.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Quality);
        for (int i = 0; i < 5; ++i)
            controller.observe(sample(16.2, 5.0));
        assert(near(controller.currentScale(), 0.95F));
        bool confirmed = false;
        for (int i = 0; i < 10; ++i) {
            controller.observe(sample(12.0, 3.5));
            confirmed = confirmed
                || controller.telemetry().reason
                    == AdaptiveFlowDecisionReason::DownstepBenefitConfirmed;
        }
        assert(confirmed);
        assert(near(controller.currentScale(), 0.95F));
    }

    {
        // #383 regression: global GPU saturation plus an output deficit is not
        // enough to justify a Flow downstep when the entire LSFG cycle is far
        // below its own generation budget. This Low-preset sample deliberately
        // clears the old flow/budget and predicted-relief gates, matching the
        // useless 0.55 -> 0.50 trial seen in the logs.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Low);
        for (int i = 0; i < 30; ++i) {
            controller.observe(sample(
                2.9, 0.9, 8.333, false, false,
                99.0, true, true, false, true));
        }
        assert(near(controller.currentScale(), 0.55F));
        assert(
            controller.telemetry().reason
                == AdaptiveFlowDecisionReason::InsufficientFlowContribution);
    }

    {
        // Under the same global pressure, Flow remains a valid actuator when
        // LSFG itself is consuming a material share of its budget and Flow is
        // a meaningful part of that constrained cycle.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Low);
        bool lowered = false;
        for (int i = 0; i < 20 && !lowered; ++i) {
            controller.observe(sample(
                7.6, 2.2, 8.333, false, false,
                99.0, true, true, false, true));
            lowered = near(controller.currentScale(), 0.50F);
        }
        assert(lowered);
    }

    {
        // Global saturation by itself must not sacrifice Flow quality when the
        // next scale step has too little scale-sensitive work to materially
        // improve the frame budget.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Quality);
        for (int i = 0; i < 30; ++i) {
            controller.observe(sample(
                8.0, 0.8, 16.666, false, false,
                99.0, true, true, false, true));
        }
        assert(near(controller.currentScale(), 1.00F));
        assert(
            controller.telemetry().reason
                == AdaptiveFlowDecisionReason::InsufficientFlowContribution);
    }

    {
        // An Adaptive-LSFG transition may hold the actuator, but sustained
        // whole-device pressure must survive that hold instead of restarting
        // its confirmation timer from zero.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Balanced);
        for (int i = 0; i < 4; ++i) {
            controller.observe(sample(
                8.0, 5.0, 16.666, false, false,
                99.0, true, true));
        }
        controller.observe(sample(
            8.0, 5.0, 16.666, true, false,
            99.0, true, true));
        bool lowered = false;
        for (int i = 0; i < 14 && !lowered; ++i) {
            controller.observe(sample(
                8.0, 5.0, 16.666, false, false,
                99.0, true, true));
            lowered = near(controller.currentScale(), 0.75F);
        }
        assert(lowered);
        for (int i = 0; i < 10; ++i) {
            controller.observe(sample(
                5.5, 1.8, 16.666, false, false,
                90.0, true, false));
        }
        assert(near(controller.currentScale(), 0.75F));
    }

    {
        // Reconfiguring presets returns to the newly selected target rather than
        // leaking the previous preset's runtime state.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Low);
        for (int i = 0; i < 12; ++i)
            controller.observe(sample(16.2, 5.0));
        controller.configure(true, AdaptiveFlowPreset::Balanced);
        assert(near(controller.currentScale(), 0.80F));
        assert(near(controller.telemetry().minimumScale, 0.55F));
    }

    {
        // Disabled mode is a strict no-op actuator.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Quality);
        controller.configure(false, AdaptiveFlowPreset::Quality);
        for (int i = 0; i < 100; ++i)
            controller.observe(sample(20.0, 8.0, 16.666, false, true));
        assert(!controller.enabled());
        assert(near(controller.currentScale(), 1.00F));
    }

    {
        // Direct LSFG GPU timing remains authoritative under repeated source
        // discontinuity/scheduler-transition observations. A stuck global GPU
        // utilization sample must not mask a grossly over-budget LSFG cycle.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Balanced);
        bool lowered = false;
        for (int i = 0; i < 20 && !lowered; ++i) {
            controller.observe(sample(
                90.0, 35.0, 65.0, true, true,
                8.0, true, true, false, true));
            lowered = near(controller.currentScale(), 0.75F);
        }
        assert(lowered);
        assert(controller.telemetry().computePressure);
    }

    {
        // A real over-budget generated sample remains actionable across the
        // source-only/history cycles used to protect Adreno from serialized
        // overload. The provenance bit distinguishes retained GPU timing from
        // arbitrary non-generated telemetry; the runtime invalidates it after
        // any Flow scale change so stale timing cannot cascade downsteps.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Balanced);
        controller.observe(sample(
            90.0, 35.0, 65.0, false, true,
            8.0, true, true, false, true));
        bool lowered = false;
        for (int i = 0; i < 12 && !lowered; ++i) {
            auto retained = sample(
                90.0, 35.0, 65.0, false, false,
                8.0, true, true, false, false);
            retained.retainedGeneratedTimingSample = true;
            controller.observe(retained);
            lowered = near(controller.currentScale(), 0.75F);
        }
        assert(lowered);
        assert(controller.telemetry().computePressure);
    }

    {
        // A sustained missed output target is direct Adaptive Flow pressure.
        // The governor must downstep without a whole-device GPU sample: that
        // sample is advisory and may be unavailable on Android.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Low);
        bool lowered = false;
        for (int i = 0; i < 20 && !lowered; ++i) {
            auto observation = sample(16.0, 5.0, 50.0);
            observation.outputCadenceValid = true;
            observation.outputTargeted = true;
            observation.outputTargetSatisfied = false;
            observation.outputDeficit = true;
            observation.outputFps = 70.0;
            observation.sourceFps = 18.0;
            controller.observe(observation);
            lowered = near(controller.currentScale(), 0.45F);
        }
        assert(lowered);
    }

    {
        // Fixed-multiplier operation uses the required generated cadence
        // (source cadence times multiplier) as the same direct output-pressure
        // signal, without requiring Adaptive LSFG or global GPU telemetry.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Low);
        bool lowered = false;
        for (int i = 0; i < 20 && !lowered; ++i) {
            auto observation = sample(16.0, 5.0, 50.0);
            observation.outputCadenceValid = true;
            observation.outputTargeted = true;
            observation.outputTargetSatisfied = false;
            observation.outputDeficit = true;
            observation.outputFps = 42.0;
            observation.sourceFps = 16.0;
            controller.observe(observation);
            lowered = near(controller.currentScale(), 0.45F);
        }
        assert(lowered);
    }


    {
        // A Flow graph transition is a control barrier, not evidence. It must
        // clear the in-flight downstep evaluation even when the transition
        // sample itself has no valid GPU timing.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Quality);
        for (int i = 0; i < 5; ++i)
            controller.observe(sample(16.2, 5.0));
        assert(near(controller.currentScale(), 0.95F));

        auto transition = sample(16.2, 5.0);
        transition.flowTransition = true;
        transition.valid = false;
        controller.observe(transition);
        assert(near(controller.currentScale(), 0.95F));
        assert(
            controller.telemetry().reason
                == AdaptiveFlowDecisionReason::FlowTransition);

        // The backend transition remains the hard barrier; the first
        // post-transition samples cannot immediately cascade another downstep.
        for (int i = 0; i < 2; ++i)
            controller.observe(sample(16.2, 5.0));
        assert(near(controller.currentScale(), 0.95F));
    }

    {
        // A target-driven downstep must not be rolled back while the target
        // remains missed. Otherwise Flow oscillates one state up/down while
        // the user-visible cadence never recovers.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Quality);
        bool lowered = false;
        for (int i = 0; i < 6 && !lowered; ++i) {
            auto observation = sample(16.0, 5.0, 50.0);
            observation.outputCadenceValid = true;
            observation.outputTargeted = true;
            observation.outputTargetSatisfied = false;
            observation.outputDeficit = true;
            observation.outputFps = 48.0;
            observation.sourceFps = 24.0;
            controller.observe(observation);
            lowered = near(controller.currentScale(), 0.90F);
        }
        assert(lowered);

        auto transition = sample(16.0, 5.0, 50.0);
        transition.flowTransition = true;
        controller.observe(transition);
        for (int i = 0; i < 40; ++i) {
            auto observation = sample(16.0, 5.0, 50.0);
            observation.outputCadenceValid = true;
            observation.outputTargeted = true;
            observation.outputTargetSatisfied = false;
            observation.outputDeficit = true;
            observation.outputFps = 48.0;
            observation.sourceFps = 24.0;
            controller.observe(observation);
        }
        assert(controller.currentScale() <= 0.90F);
        assert(controller.telemetry().reason
            != AdaptiveFlowDecisionReason::DownstepReverted);
    }


    {
        // A target-driven downstep must remain committed while the measured
        // output target is still missed even if the deficit debounce clears
        // for a short cadence window. This is the exact runtime case that
        // previously bounced 0.95 -> 1.00 with output_satisfied=0.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Quality);
        bool lowered = false;
        for (int i = 0; i < 8 && !lowered; ++i) {
            auto observation = sample(16.0, 5.0, 50.0);
            observation.outputCadenceValid = true;
            observation.outputTargeted = true;
            observation.outputTargetSatisfied = false;
            observation.outputDeficit = true;
            observation.outputFps = 48.0;
            controller.observe(observation);
            lowered = near(controller.currentScale(), 0.90F);
        }
        assert(lowered);

        for (int i = 0; i < 8; ++i) {
            auto observation = sample(15.0, 4.5, 50.0);
            observation.outputCadenceValid = true;
            observation.outputTargeted = true;
            observation.outputTargetSatisfied = false;
            observation.outputDeficit = false;
            observation.outputFps = 49.0;
            controller.observe(observation);
        }
        assert(controller.currentScale() <= 0.90F);
        assert(controller.telemetry().reason
            != AdaptiveFlowDecisionReason::DownstepReverted);
    }


    {
        // A trustworthy fixed source target remains authoritative even when
        // the next 0.05 step has little predicted standalone relief. Keep
        // lowering until the source target recovers or the Auto floor is hit.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Auto);
        for (int i = 0; i < 200 && !near(controller.currentScale(), 0.25F); ++i) {
            auto observation = sample(8.0, 0.6, 50.0);
            observation.fixedMultiplierMode = true;
            observation.fixedMultiplierBaseTarget = true;
            observation.sourceFps = 20.0;
            observation.sourceTargetFps = 30.0;
            controller.observe(observation);
        }
        assert(near(controller.currentScale(), 0.25F));
    }

    {
        // Fixed mode can start generating before a trustworthy source-only
        // baseline exists. Bootstrap with one bounded Flow probe and continue
        // only while source cadence measurably improves.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Auto);
        for (int i = 0; i < 8; ++i) {
            auto observation = sample(8.0, 5.0, 50.0);
            observation.fixedMultiplierMode = true;
            observation.sourceFps =
                i < 3 ? 20.0 : (i < 6 ? 22.0 : 24.0);
            controller.observe(observation);
        }
        assert(controller.currentScale() <= 0.90F);
        assert(controller.telemetry().sourceReferenceFps >= 21.9);
    }

    {
        // Fixed LSFG can begin generating before a clean source-only baseline
        // ever exists. After exploratory downscaling finds a useful state and
        // a subsequent lower probe fails, sustained headroom must still allow
        // quality recovery without sourceTargetFps.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Auto);

        bool firstStep = false;
        for (int i = 0; i < 12 && !firstStep; ++i) {
            auto observation = sample(8.0, 5.0, 50.0);
            observation.fixedMultiplierMode = true;
            observation.sourceFps = 20.0;
            controller.observe(observation);
            firstStep = near(controller.currentScale(), 0.95F);
        }
        assert(firstStep);

        auto firstTransition = sample(8.0, 5.0, 50.0);
        firstTransition.fixedMultiplierMode = true;
        firstTransition.sourceFps = 22.0;
        firstTransition.flowTransition = true;
        controller.observe(firstTransition);
        for (int i = 0; i < 3; ++i) {
            auto settle = sample(8.0, 5.0, 50.0);
            settle.fixedMultiplierMode = true;
            settle.sourceFps = 22.0;
            controller.observe(settle);
        }

        bool firstBenefit = false;
        for (int i = 0; i < 12 && !firstBenefit; ++i) {
            auto observation = sample(7.0, 4.0, 50.0);
            observation.fixedMultiplierMode = true;
            observation.sourceFps = 22.0;
            controller.observe(observation);
            firstBenefit = controller.telemetry().reason
                == AdaptiveFlowDecisionReason::DownstepBenefitConfirmed;
        }
        assert(firstBenefit);

        bool secondStep = false;
        for (int i = 0; i < 12 && !secondStep; ++i) {
            auto observation = sample(7.0, 4.0, 50.0);
            observation.fixedMultiplierMode = true;
            observation.sourceFps = 22.0;
            controller.observe(observation);
            secondStep = near(controller.currentScale(), 0.90F);
        }
        assert(secondStep);

        auto secondTransition = sample(7.0, 4.0, 50.0);
        secondTransition.fixedMultiplierMode = true;
        secondTransition.sourceFps = 22.0;
        secondTransition.flowTransition = true;
        controller.observe(secondTransition);
        for (int i = 0; i < 3; ++i) {
            auto settle = sample(7.0, 4.0, 50.0);
            settle.fixedMultiplierMode = true;
            settle.sourceFps = 22.0;
            controller.observe(settle);
        }

        bool reverted = false;
        for (int i = 0; i < 16 && !reverted; ++i) {
            auto observation = sample(7.0, 4.0, 50.0);
            observation.fixedMultiplierMode = true;
            observation.sourceFps = 22.1;
            controller.observe(observation);
            reverted = controller.telemetry().reason
                == AdaptiveFlowDecisionReason::DownstepReverted;
        }
        assert(reverted);
        assert(near(controller.currentScale(), 0.95F));

        bool recovered = false;
        for (int i = 0; i < 80 && !recovered; ++i) {
            auto observation = sample(5.0, 1.0, 50.0);
            observation.fixedMultiplierMode = true;
            observation.sourceFps = 30.0;
            controller.observe(observation);
            recovered = near(controller.currentScale(), 1.00F);
        }
        assert(recovered);
    }

    {
        // A missing-baseline exploratory step that does not recover source
        // cadence is reverted instead of walking quality to the minimum.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Auto);
        for (int i = 0; i < 8; ++i) {
            auto observation = sample(8.0, 5.0, 50.0);
            observation.fixedMultiplierMode = true;
            observation.sourceFps = 30.0;
            controller.observe(observation);
        }
        assert(near(controller.currentScale(), 1.00F));
    }

    {
        // Fixed multiplier mode must protect the clean source cadence even if
        // generated output currently appears to meet its output-rate target.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Quality);
        bool lowered = false;
        for (int i = 0; i < 8 && !lowered; ++i) {
            auto observation = sample(16.0, 5.0, 50.0);
            observation.outputCadenceValid = true;
            observation.outputTargeted = true;
            observation.outputTargetSatisfied = true;
            observation.outputDeficit = false;
            observation.outputFps = 60.0;
            observation.fixedMultiplierBaseTarget = true;
            observation.sourceFps = 22.0;
            observation.sourceTargetFps = 30.0;
            controller.observe(observation);
            lowered = near(controller.currentScale(), 0.90F);
        }
        assert(lowered);
        for (int i = 0; i < 50; ++i) {
            auto observation = sample(8.0, 2.0, 50.0);
            observation.outputCadenceValid = true;
            observation.outputTargeted = true;
            observation.outputTargetSatisfied = true;
            observation.outputDeficit = false;
            observation.outputFps = 60.0;
            observation.fixedMultiplierBaseTarget = true;
            observation.sourceFps = 22.0;
            observation.sourceTargetFps = 30.0;
            controller.observe(observation);
        }
        assert(controller.currentScale() <= 0.95F);
        const float sourceDeficitScale = controller.currentScale();
        for (int i = 0; i < 60; ++i) {
            auto observation = sample(8.0, 2.0, 50.0);
            observation.outputCadenceValid = true;
            observation.outputTargeted = true;
            observation.outputTargetSatisfied = true;
            observation.outputFps = 60.0;
            observation.fixedMultiplierBaseTarget = true;
            observation.sourceFps = 30.0;
            observation.sourceTargetFps = 30.0;
            controller.observe(observation);
        }
        assert(controller.currentScale() > sourceDeficitScale);
    }

    {
        // A fixed multiplier can request an output rate above the display
        // cadence. That output deficit must not downscale Flow while the source
        // remains at its clean baseline.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Quality);
        for (int i = 0; i < 40; ++i) {
            auto observation = sample(
                8.0, 2.0, 50.0, false, false, 80.0, true, true);
            observation.outputCadenceValid = true;
            observation.outputTargeted = true;
            observation.outputTargetSatisfied = false;
            observation.fixedMultiplierBaseTarget = true;
            observation.sourceFps = 30.0;
            observation.sourceTargetFps = 30.0;
            controller.observe(observation);
        }

        assert(near(controller.currentScale(), 1.00F));
        assert(!controller.telemetry().outputPressure);
        assert(!controller.telemetry().sourcePressure);
    }

    {
        // Once source pacing recovers, Flow must be allowed to rise even if
        // the display still cannot show the fixed multiplier's full output.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Quality);
        bool lowered = false;
        for (int i = 0; i < 30 && !lowered; ++i) {
            auto observation = sample(8.0, 5.0, 50.0);
            observation.outputCadenceValid = true;
            observation.outputTargeted = true;
            observation.outputTargetSatisfied = true;
            observation.outputFps = 60.0;
            observation.fixedMultiplierBaseTarget = true;
            observation.sourceFps = 22.0;
            observation.sourceTargetFps = 30.0;
            controller.observe(observation);
            lowered = controller.currentScale() < 1.00F;
        }
        assert(lowered);
        const float sourceDeficitScale = controller.currentScale();

        for (int i = 0; i < 100; ++i) {
            auto observation = sample(8.0, 2.0, 50.0, false, false, 80.0, true, true);
            observation.outputCadenceValid = true;
            observation.outputTargeted = true;
            observation.outputTargetSatisfied = false;
            observation.outputFps = 60.0;
            observation.fixedMultiplierBaseTarget = true;
            observation.sourceFps = 30.0;
            observation.sourceTargetFps = 30.0;
            controller.observe(observation);
        }

        assert(controller.currentScale() > sourceDeficitScale);
        assert(!controller.telemetry().outputPressure);
        assert(!controller.telemetry().sourcePressure);
    }

    {
        // Severe thermal state is advisory only. With no source/output/LSFG
        // pressure it must never lower Flow Scale.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Quality);
        for (int i = 0; i < 20; ++i) {
            auto observation = sample(8.0, 2.0, 16.666);
            observation.thermalPressureValid = true;
            observation.thermalStatus = 4;
            controller.observe(observation);
        }
        assert(near(controller.currentScale(), 1.00F));
        assert(controller.telemetry().thermalPressure);
    }

    {
        // Once real LSFG compute pressure exists, severe thermal state may
        // accelerate confirmation. The same three 100 ms samples are below
        // the ordinary 400 ms confirmation interval.
        AdaptiveFlowController normal(AdaptiveFlowPreset::Quality);
        for (int i = 0; i < 3; ++i)
            normal.observe(sample(16.2, 5.0));
        assert(near(normal.currentScale(), 1.00F));

        AdaptiveFlowController thermal(AdaptiveFlowPreset::Quality);
        for (int i = 0; i < 3; ++i) {
            auto observation = sample(16.2, 5.0);
            observation.thermalPressureValid = true;
            observation.thermalStatus = 4;
            thermal.observe(observation);
        }
        assert(near(thermal.currentScale(), 0.95F));
        assert(thermal.telemetry().thermalPressure);
    }

    return 0;
}
