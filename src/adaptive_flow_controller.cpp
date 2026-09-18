#include "adaptive_flow_controller.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace {
constexpr std::array<float, 4> kQualityStates{1.00F, 0.90F, 0.80F, 0.70F};
constexpr std::array<float, 4> kBalancedStates{0.80F, 0.70F, 0.625F, 0.55F};
constexpr std::array<float, 4> kLowStates{0.55F, 0.45F, 0.35F, 0.25F};

constexpr double kPressureRatio = 0.90;
constexpr double kRecoveryRatio = 0.72;
constexpr double kRecoveryPredictedRatio = 0.82;
constexpr double kMinimumFlowBudgetRatio = 0.10;
constexpr double kMinimumPredictedReliefRatio = 0.03;
constexpr double kDownConfirmSeconds = 0.90;
constexpr double kUpConfirmSeconds = 4.0;
constexpr double kTransitionCooldownSeconds = 1.25;
constexpr double kSchedulerTransitionHoldSeconds = 1.25;

std::span<const float> states(AdaptiveFlowPreset preset) {
    switch (preset) {
    case AdaptiveFlowPreset::Quality: return kQualityStates;
    case AdaptiveFlowPreset::Balanced: return kBalancedStates;
    case AdaptiveFlowPreset::Low: return kLowStates;
    }
    return kQualityStates;
}

} // namespace

AdaptiveFlowController::AdaptiveFlowController(AdaptiveFlowPreset preset)
        : enabled_(true), preset_(preset) {
    selectTargetState();
}

void AdaptiveFlowController::configure(bool enabled, AdaptiveFlowPreset preset) {
    if (enabled_ == enabled && preset_ == preset)
        return;
    enabled_ = enabled;
    preset_ = preset;
    observedSeconds_ = 0.0;
    cooldownUntilSeconds_ = 0.0;
    schedulerHoldUntilSeconds_ = 0.0;
    resetEvidence();
    selectTargetState();
    if (!enabled_)
        telemetry_.reason = AdaptiveFlowDecisionReason::Disabled;
}

void AdaptiveFlowController::selectTargetState() {
    const auto presetStates = states(preset_);
    telemetry_ = {};
    telemetry_.targetScale = presetStates.front();
    telemetry_.minimumScale = presetStates.back();
    telemetry_.currentScale = presetStates.front();
    telemetry_.stateIndex = 0;
}

void AdaptiveFlowController::resetEvidence() {
    pressureSeconds_ = 0.0;
    headroomSeconds_ = 0.0;
}

void AdaptiveFlowController::reset() {
    observedSeconds_ = 0.0;
    cooldownUntilSeconds_ = 0.0;
    schedulerHoldUntilSeconds_ = 0.0;
    resetEvidence();
    selectTargetState();
    if (!enabled_)
        telemetry_.reason = AdaptiveFlowDecisionReason::Disabled;
}

float AdaptiveFlowController::observe(const AdaptiveFlowObservation& observation) {
    telemetry_.changed = false;
    telemetry_.estimatedNextTotalMs = 0.0;

    if (!enabled_) {
        telemetry_.reason = AdaptiveFlowDecisionReason::Disabled;
        return telemetry_.currentScale;
    }

    const double elapsedSeconds = std::chrono::duration<double>(observation.elapsed).count();
    const double evidenceSeconds =
        elapsedSeconds > 0.0 && std::isfinite(elapsedSeconds)
            ? std::min(elapsedSeconds, 0.250)
            : 0.0;
    observedSeconds_ += evidenceSeconds;

    if (!observation.valid
            || !(observation.frameBudgetMs > 0.0)
            || !std::isfinite(observation.frameBudgetMs)
            || observation.totalLsfgMs < 0.0
            || observation.flowMs < 0.0
            || !std::isfinite(observation.totalLsfgMs)
            || !std::isfinite(observation.flowMs)) {
        resetEvidence();
        telemetry_.reason = AdaptiveFlowDecisionReason::InvalidTelemetry;
        return telemetry_.currentScale;
    }

    telemetry_.pressureRatio = observation.totalLsfgMs / observation.frameBudgetMs;
    telemetry_.flowBudgetRatio = observation.flowMs / observation.frameBudgetMs;

    if (observation.schedulerTransition) {
        schedulerHoldUntilSeconds_ = std::max(
            schedulerHoldUntilSeconds_, observedSeconds_ + kSchedulerTransitionHoldSeconds);
        resetEvidence();
        telemetry_.reason = AdaptiveFlowDecisionReason::SchedulerTransition;
        return telemetry_.currentScale;
    }

    if (observedSeconds_ < schedulerHoldUntilSeconds_) {
        resetEvidence();
        telemetry_.reason = AdaptiveFlowDecisionReason::SchedulerTransition;
        return telemetry_.currentScale;
    }

    if (observedSeconds_ < cooldownUntilSeconds_) {
        resetEvidence();
        telemetry_.reason = AdaptiveFlowDecisionReason::Cooldown;
        return telemetry_.currentScale;
    }

    const auto presetStates = states(preset_);
    const std::size_t index = telemetry_.stateIndex;
    const bool canLower = index + 1 < presetStates.size();
    const bool canRaise = index > 0;

    const bool pressure = observation.deadlineMissed
        || telemetry_.pressureRatio >= kPressureRatio;

    if (pressure && canLower) {
        const double currentScale = static_cast<double>(presetStates[index]);
        const double lowerScale = static_cast<double>(presetStates[index + 1]);
        const double scaleWorkRatio = (lowerScale * lowerScale) / (currentScale * currentScale);
        const double predictedReliefMs = observation.flowMs * (1.0 - scaleWorkRatio);
        const double predictedReliefRatio = predictedReliefMs / observation.frameBudgetMs;

        if (telemetry_.flowBudgetRatio < kMinimumFlowBudgetRatio
                || predictedReliefRatio < kMinimumPredictedReliefRatio) {
            resetEvidence();
            telemetry_.reason = AdaptiveFlowDecisionReason::InsufficientFlowContribution;
            return telemetry_.currentScale;
        }

        pressureSeconds_ += evidenceSeconds;
        headroomSeconds_ = 0.0;
        if (pressureSeconds_ >= kDownConfirmSeconds) {
            telemetry_.stateIndex++;
            telemetry_.currentScale = presetStates[telemetry_.stateIndex];
            telemetry_.changed = true;
            telemetry_.reason = AdaptiveFlowDecisionReason::SustainedPressure;
            cooldownUntilSeconds_ = observedSeconds_ + kTransitionCooldownSeconds;
            resetEvidence();
        } else {
            telemetry_.reason = AdaptiveFlowDecisionReason::None;
        }
        return telemetry_.currentScale;
    }

    pressureSeconds_ = 0.0;

    if (canRaise && telemetry_.pressureRatio <= kRecoveryRatio && !observation.deadlineMissed) {
        const double currentScale = static_cast<double>(presetStates[index]);
        const double higherScale = static_cast<double>(presetStates[index - 1]);
        const double addedFlowMs = observation.flowMs
            * ((higherScale * higherScale) / (currentScale * currentScale) - 1.0);
        const double predictedTotalMs = observation.totalLsfgMs + addedFlowMs;
        telemetry_.estimatedNextTotalMs = predictedTotalMs;

        if (predictedTotalMs / observation.frameBudgetMs > kRecoveryPredictedRatio) {
            headroomSeconds_ = 0.0;
            telemetry_.reason = AdaptiveFlowDecisionReason::InsufficientRecoveryHeadroom;
            return telemetry_.currentScale;
        }

        headroomSeconds_ += evidenceSeconds;
        if (headroomSeconds_ >= kUpConfirmSeconds) {
            telemetry_.stateIndex--;
            telemetry_.currentScale = presetStates[telemetry_.stateIndex];
            telemetry_.changed = true;
            telemetry_.reason = AdaptiveFlowDecisionReason::SustainedHeadroom;
            cooldownUntilSeconds_ = observedSeconds_ + kTransitionCooldownSeconds;
            resetEvidence();
        } else {
            telemetry_.reason = AdaptiveFlowDecisionReason::None;
        }
        return telemetry_.currentScale;
    }

    headroomSeconds_ = 0.0;
    telemetry_.reason = canRaise
        ? AdaptiveFlowDecisionReason::InsufficientRecoveryHeadroom
        : AdaptiveFlowDecisionReason::None;
    return telemetry_.currentScale;
}

std::span<const float> AdaptiveFlowController::statesForPreset(AdaptiveFlowPreset preset) {
    return states(preset);
}

const char* AdaptiveFlowController::presetName(AdaptiveFlowPreset preset) {
    switch (preset) {
    case AdaptiveFlowPreset::Quality: return "quality";
    case AdaptiveFlowPreset::Balanced: return "balanced";
    case AdaptiveFlowPreset::Low: return "low";
    }
    return "quality";
}

const char* AdaptiveFlowController::reasonName(AdaptiveFlowDecisionReason reason) {
    switch (reason) {
    case AdaptiveFlowDecisionReason::None: return "none";
    case AdaptiveFlowDecisionReason::Disabled: return "disabled";
    case AdaptiveFlowDecisionReason::InvalidTelemetry: return "invalid_telemetry";
    case AdaptiveFlowDecisionReason::SchedulerTransition: return "adaptive_lsfg_transition";
    case AdaptiveFlowDecisionReason::Cooldown: return "cooldown";
    case AdaptiveFlowDecisionReason::InsufficientFlowContribution: return "insufficient_flow_contribution";
    case AdaptiveFlowDecisionReason::SustainedPressure: return "sustained_gpu_pressure";
    case AdaptiveFlowDecisionReason::InsufficientRecoveryHeadroom: return "insufficient_recovery_headroom";
    case AdaptiveFlowDecisionReason::SustainedHeadroom: return "sustained_headroom";
    }
    return "none";
}
