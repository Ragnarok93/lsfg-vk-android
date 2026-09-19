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
        const auto quality = AdaptiveFlowController::statesForPreset(AdaptiveFlowPreset::Quality);
        const auto balanced = AdaptiveFlowController::statesForPreset(AdaptiveFlowPreset::Balanced);
        const auto low = AdaptiveFlowController::statesForPreset(AdaptiveFlowPreset::Low);
        assert(quality.size() == 4 && near(quality.front(), 1.00F) && near(quality.back(), 0.70F));
        assert(balanced.size() == 4 && near(balanced.front(), 0.80F) && near(balanced.back(), 0.55F));
        assert(low.size() == 4 && near(low.front(), 0.55F) && near(low.back(), 0.25F));
    }

    {
        // Adaptive always starts at the preset target and does not move for a
        // short pressure burst.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Quality);
        assert(near(controller.currentScale(), 1.00F));
        for (int i = 0; i < 8; ++i)
            controller.observe(sample(16.0, 5.0));
        assert(near(controller.currentScale(), 1.00F));
    }

    {
        // Sustained pressure with a material scale-sensitive contribution lowers
        // one state, then observes a cooldown instead of cascading immediately.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Quality);
        for (int i = 0; i < 12; ++i)
            controller.observe(sample(16.2, 5.0));
        assert(near(controller.currentScale(), 0.90F));
        assert(controller.telemetry().reason == AdaptiveFlowDecisionReason::SustainedPressure
            || controller.telemetry().reason == AdaptiveFlowDecisionReason::Cooldown);
        for (int i = 0; i < 8; ++i)
            controller.observe(sample(16.2, 5.0));
        assert(near(controller.currentScale(), 0.90F));
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
        for (int i = 0; i < 9; ++i)
            controller.observe(sample(16.2, 5.0));
        controller.observe(sample(16.2, 5.0, 16.666, true));
        for (int i = 0; i < 10; ++i)
            controller.observe(sample(16.2, 5.0));
        assert(near(controller.currentScale(), 0.80F));
        for (int i = 0; i < 12; ++i)
            controller.observe(sample(16.2, 5.0));
        assert(near(controller.currentScale(), 0.70F));
    }

    {
        // Degradation never passes the preset hard floor.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Low);
        for (int step = 0; step < 4; ++step) {
            for (int i = 0; i < 30; ++i)
                controller.observe(sample(16.4, 7.0, 16.666, false, true));
        }
        assert(near(controller.currentScale(), 0.25F));
    }

    {
        // Recovery is intentionally slower and only happens when the estimated
        // next quality state still fits comfortably in budget.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Quality);
        for (int i = 0; i < 12; ++i)
            controller.observe(sample(16.2, 5.0));
        assert(near(controller.currentScale(), 0.90F));
        for (int i = 0; i < 30; ++i)
            controller.observe(sample(8.0, 2.0));
        assert(near(controller.currentScale(), 0.90F));
        for (int i = 0; i < 20; ++i)
            controller.observe(sample(8.0, 2.0));
        assert(near(controller.currentScale(), 1.00F));
    }

    {
        // Recovery should be quality-seeking: once the higher state is
        // predicted to fit comfortably, the lower state's raw utilization
        // ratio must not strand quality indefinitely.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Quality);
        for (int i = 0; i < 12; ++i)
            controller.observe(sample(16.2, 5.0));
        assert(near(controller.currentScale(), 0.90F));

        for (int i = 0; i < 70; ++i)
            controller.observe(sample(12.5, 1.0));
        assert(near(controller.currentScale(), 1.00F));
    }

    {
        // If a higher state is predicted to consume too much of the budget,
        // hold the lower state despite otherwise healthy current timings.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Quality);
        for (int i = 0; i < 12; ++i)
            controller.observe(sample(16.2, 6.0));
        assert(near(controller.currentScale(), 0.90F));
        for (int i = 0; i < 60; ++i)
            controller.observe(sample(11.8, 9.0));
        assert(near(controller.currentScale(), 0.90F));
    }

    {
        // A suspend-sized observation must not satisfy several seconds of
        // recovery evidence in one call. This matters for Adaptive Flow paired
        // with Fixed LSFG, where no Adaptive-LSFG discontinuity event exists.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Quality);
        for (int i = 0; i < 12; ++i)
            controller.observe(sample(16.2, 5.0));
        assert(near(controller.currentScale(), 0.90F));

        for (int i = 0; i < 20; ++i)
            controller.observe(sample(8.0, 2.0));
        auto resumed = sample(8.0, 2.0);
        resumed.elapsed = 10s;
        controller.observe(resumed);
        assert(near(controller.currentScale(), 0.90F));
    }

    {
        // Whole-device GPU saturation plus a real output deficit must lower
        // Flow Scale even when the current cycle is history-only. The retained
        // generated-work timing supplies the scale-sensitive relief estimate.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Quality);
        for (int i = 0; i < 6; ++i) {
            controller.observe(sample(
                8.0, 3.0, 16.666, false, false,
                99.0, true, true, false, false));
        }
        assert(near(controller.currentScale(), 0.90F));
        assert(
            controller.telemetry().reason
                == AdaptiveFlowDecisionReason::SustainedGlobalPressure
            || controller.telemetry().reason
                == AdaptiveFlowDecisionReason::Cooldown);
    }

    {
        // History-only cycles can never prove recovery headroom. After global
        // pressure lowers the scale, many cheap zero-generation samples must
        // not immediately raise quality again.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Quality);
        for (int i = 0; i < 6; ++i) {
            controller.observe(sample(
                8.0, 3.0, 16.666, false, false,
                99.0, true, true, false, false));
        }
        assert(near(controller.currentScale(), 0.90F));

        for (int i = 0; i < 70; ++i) {
            controller.observe(sample(
                5.0, 1.5, 16.666, false, false,
                45.0, true, false, false, false));
        }
        assert(near(controller.currentScale(), 0.90F));
    }

    {
        // An Adaptive-LSFG transition may hold the actuator, but sustained
        // whole-device pressure must survive that hold instead of restarting
        // its confirmation timer from zero.
        AdaptiveFlowController controller(AdaptiveFlowPreset::Balanced);
        for (int i = 0; i < 4; ++i) {
            controller.observe(sample(
                8.0, 3.0, 16.666, false, false,
                99.0, true, true));
        }
        controller.observe(sample(
            8.0, 3.0, 16.666, true, false,
            99.0, true, true));
        for (int i = 0; i < 14; ++i) {
            controller.observe(sample(
                8.0, 3.0, 16.666, false, false,
                99.0, true, true));
        }
        assert(near(controller.currentScale(), 0.70F));
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

    return 0;
}
