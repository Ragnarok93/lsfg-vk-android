#include "adaptive_flow_controller.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace {
constexpr std::array<float, 4> kQualityStates{1.00F, 0.90F, 0.80F, 0.70F};
constexpr std::array<float, 4> kBalancedStates{0.80F, 0.70F, 0.625F, 0.55F};
constexpr std::array<float, 4> kLowStates{0.55F, 0.45F, 0.35F, 0.25F};

constexpr double kPressureRatio = 0.90;
constexpr double kRecoveryPredictedRatio = 0.82;
constexpr double kMinimumFlowBudgetRatio = 0.10;
constexpr double kMinimumPredictedReliefRatio = 0.03;
constexpr double kMinimumGlobalPressureLsfgBudgetRatio = 0.40;
constexpr double kDownConfirmSeconds = 0.90;
constexpr double kGlobalDownConfirmSeconds = 0.50;
constexpr double kUpConfirmSeconds = 4.0;
constexpr double kGlobalGpuPressurePercent = 96.0;
constexpr double kGlobalGpuRecoveryPercent = 88.0;
constexpr double kTransitionCooldownSeconds = 1.25;
constexpr double kSchedulerTransitionHoldSeconds = 1.25;
constexpr double kDownstepEvaluationSeconds = 0.80;
constexpr double kDownstepNoBenefitHoldSeconds = 4.0;
constexpr double kMaterialPressureRatioRelief = 0.05;
constexpr double kMaterialOutputGainRatio = 1.02;
constexpr double kMaterialSourceGainRatio = 1.03;
constexpr double kMaterialFlowReliefRatio = 0.88;
constexpr double kMaterialTotalReliefRatio = 0.95;
constexpr double kMaterialWsiReliefRatio = 0.75;
constexpr double kMaterialGlobalGpuReliefPercent = 3.0;

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
    downstepEvaluationActive_ = false;
    downstepBenefitSeen_ = false;
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
    downstepEvaluationActive_ = false;
    downstepBenefitSeen_ = false;
    resetEvidence();
    selectTargetState();
    if (!enabled_)
        telemetry_.reason = AdaptiveFlowDecisionReason::Disabled;
}

float AdaptiveFlowController::observe(const AdaptiveFlowObservation& observation) {
    telemetry_.changed = false;
    telemetry_.estimatedNextTotalMs = 0.0;
    telemetry_.pressureRatio = 0.0;
    telemetry_.flowBudgetRatio = 0.0;
    telemetry_.globalGpuUsagePercent = 0.0;
    telemetry_.globalPressure = false;
    telemetry_.computePressure = false;
    telemetry_.wsiPressure = false;
    telemetry_.outputDeficit = false;
    telemetry_.downstepEvaluationActive = downstepEvaluationActive_;

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
        if (downstepEvaluationActive_)
            downstepEvaluationStartedSeconds_ += evidenceSeconds;
        resetEvidence();
        telemetry_.downstepEvaluationActive = downstepEvaluationActive_;
        telemetry_.reason = AdaptiveFlowDecisionReason::InvalidTelemetry;
        return telemetry_.currentScale;
    }

    telemetry_.pressureRatio = observation.totalLsfgMs / observation.frameBudgetMs;
    telemetry_.flowBudgetRatio = observation.flowMs / observation.frameBudgetMs;
    telemetry_.globalGpuUsagePercent = observation.globalPressureValid
        ? observation.globalGpuUsagePercent : 0.0;
    telemetry_.outputDeficit = observation.outputDeficit;
    const bool computePressure =
        observation.generatedWorkSample
        && (observation.computeDeadlinePressure
            || observation.deadlineMissed
            || telemetry_.pressureRatio >= kPressureRatio);
    const bool wsiPressure = observation.wsiPresentationPressure;
    const bool globalGpuPressure =
        observation.globalPressureValid
        && observation.globalGpuUsagePercent >= kGlobalGpuPressurePercent;
    const bool globalPressure =
        globalGpuPressure
        && (observation.outputDeficit || observation.syntheticDropPressure);
    const bool wsiFlowPressure =
        wsiPressure && globalGpuPressure && observation.outputDeficit;
    telemetry_.computePressure = computePressure;
    telemetry_.wsiPressure = wsiPressure;
    telemetry_.globalPressure = globalPressure;

    if (observation.schedulerTransition) {
        if (downstepEvaluationActive_)
            downstepEvaluationStartedSeconds_ += evidenceSeconds;
        schedulerHoldUntilSeconds_ = std::max(
            schedulerHoldUntilSeconds_, observedSeconds_ + kSchedulerTransitionHoldSeconds);
        headroomSeconds_ = 0.0;
        if (globalPressure)
            pressureSeconds_ += evidenceSeconds;
        else
            pressureSeconds_ = 0.0;
        telemetry_.reason = AdaptiveFlowDecisionReason::SchedulerTransition;
        return telemetry_.currentScale;
    }

    if (observedSeconds_ < schedulerHoldUntilSeconds_) {
        if (downstepEvaluationActive_)
            downstepEvaluationStartedSeconds_ += evidenceSeconds;
        headroomSeconds_ = 0.0;
        if (globalPressure)
            pressureSeconds_ += evidenceSeconds;
        else
            pressureSeconds_ = 0.0;
        telemetry_.reason = AdaptiveFlowDecisionReason::SchedulerTransition;
        return telemetry_.currentScale;
    }

    if (downstepEvaluationActive_) {
        const bool outputBenefit =
            downstepBaselineOutputValid_
            && observation.outputCadenceValid
            && observation.outputFps
                >= downstepBaselineOutputFps_ * kMaterialOutputGainRatio;
        const bool sourceBenefit =
            downstepBaselineSourceFps_ > 0.0
            && observation.sourceFps
                >= downstepBaselineSourceFps_ * kMaterialSourceGainRatio;
        const bool computeBenefit =
            downstepBaselineComputePressure_
            && observation.generatedWorkSample
            && (!computePressure
                || telemetry_.pressureRatio
                    <= downstepBaselinePressureRatio_
                        - kMaterialPressureRatioRelief
                || (observation.flowMs
                        <= downstepBaselineFlowMs_
                            * kMaterialFlowReliefRatio
                    && observation.totalLsfgMs
                        <= downstepBaselineTotalMs_
                            * kMaterialTotalReliefRatio));
        const bool wsiBenefit =
            downstepBaselineWsiPressure_
            && downstepBaselineWsiLossRate_ > 0.0
            && observation.wsiLossRate
                <= downstepBaselineWsiLossRate_
                    * kMaterialWsiReliefRatio;
        const bool globalBenefit =
            downstepBaselineGlobalPressure_
            && observation.globalPressureValid
            && observation.globalGpuUsagePercent
                <= downstepBaselineGlobalGpuPercent_
                    - kMaterialGlobalGpuReliefPercent;

        downstepBenefitSeen_ = downstepBenefitSeen_
            || outputBenefit || sourceBenefit || computeBenefit
            || wsiBenefit || globalBenefit;

        if (observedSeconds_ - downstepEvaluationStartedSeconds_
                < kDownstepEvaluationSeconds) {
            resetEvidence();
            telemetry_.downstepEvaluationActive = true;
            telemetry_.reason = AdaptiveFlowDecisionReason::EvaluatingDownstep;
            return telemetry_.currentScale;
        }

        downstepEvaluationActive_ = false;
        telemetry_.downstepEvaluationActive = false;
        resetEvidence();
        if (downstepBenefitSeen_) {
            downstepBenefitSeen_ = false;
            telemetry_.reason =
                AdaptiveFlowDecisionReason::DownstepBenefitConfirmed;
            return telemetry_.currentScale;
        }

        const auto presetStates = states(preset_);
        telemetry_.stateIndex = std::min(
            downstepPreviousIndex_, presetStates.size() - 1);
        telemetry_.currentScale = presetStates[telemetry_.stateIndex];
        telemetry_.changed = true;
        telemetry_.reason = AdaptiveFlowDecisionReason::DownstepReverted;
        cooldownUntilSeconds_ =
            observedSeconds_ + kDownstepNoBenefitHoldSeconds;
        downstepBenefitSeen_ = false;
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

    const bool pressure =
        computePressure || globalPressure || wsiFlowPressure;

    if (pressure && canLower) {
        const double currentScale = static_cast<double>(presetStates[index]);
        const double lowerScale = static_cast<double>(presetStates[index + 1]);
        const double scaleWorkRatio = (lowerScale * lowerScale) / (currentScale * currentScale);
        const double predictedReliefMs = observation.flowMs * (1.0 - scaleWorkRatio);
        const double predictedReliefRatio = predictedReliefMs / observation.frameBudgetMs;

        // Whole-device saturation says the system is under pressure; it does
        // not prove Flow is large enough to be a useful actuator. Keep the same
        // material-contribution gate under local and global pressure so quality
        // is never traded for a few tenths of a millisecond that cannot
        // plausibly recover the requested cadence.
        const bool globalOnlyPressure = globalPressure && !computePressure;
        const bool globallyProfitable =
            !globalOnlyPressure
            || telemetry_.pressureRatio >= kMinimumGlobalPressureLsfgBudgetRatio;
        if (telemetry_.flowBudgetRatio < kMinimumFlowBudgetRatio
                || predictedReliefRatio < kMinimumPredictedReliefRatio
                || !globallyProfitable) {
            resetEvidence();
            telemetry_.reason = AdaptiveFlowDecisionReason::InsufficientFlowContribution;
            return telemetry_.currentScale;
        }

        pressureSeconds_ += evidenceSeconds;
        headroomSeconds_ = 0.0;
        const double downConfirmSeconds = globalPressure
            ? kGlobalDownConfirmSeconds : kDownConfirmSeconds;
        if (pressureSeconds_ >= downConfirmSeconds) {
            downstepEvaluationActive_ = true;
            downstepBenefitSeen_ = false;
            downstepPreviousIndex_ = index;
            downstepEvaluationStartedSeconds_ = observedSeconds_;
            downstepBaselinePressureRatio_ = telemetry_.pressureRatio;
            downstepBaselineFlowMs_ = observation.flowMs;
            downstepBaselineTotalMs_ = observation.totalLsfgMs;
            downstepBaselineSourceFps_ = observation.sourceFps;
            downstepBaselineOutputFps_ = observation.outputFps;
            downstepBaselineWsiLossRate_ = observation.wsiLossRate;
            downstepBaselineGlobalGpuPercent_ =
                observation.globalGpuUsagePercent;
            downstepBaselineOutputValid_ =
                observation.outputCadenceValid;
            downstepBaselineComputePressure_ = computePressure;
            downstepBaselineWsiPressure_ = wsiPressure;
            downstepBaselineGlobalPressure_ = globalPressure;
            telemetry_.stateIndex++;
            telemetry_.currentScale = presetStates[telemetry_.stateIndex];
            telemetry_.changed = true;
            telemetry_.downstepEvaluationActive = true;
            telemetry_.reason = globalPressure
                ? AdaptiveFlowDecisionReason::SustainedGlobalPressure
                : AdaptiveFlowDecisionReason::SustainedPressure;
            cooldownUntilSeconds_ = observedSeconds_ + kTransitionCooldownSeconds;
            resetEvidence();
        } else {
            telemetry_.reason = AdaptiveFlowDecisionReason::None;
        }
        return telemetry_.currentScale;
    }

    pressureSeconds_ = 0.0;

    const bool globalRecoveryHeadroom =
        !observation.globalPressureValid
        || observation.globalGpuUsagePercent <= kGlobalGpuRecoveryPercent;
    const bool outputRecoverySatisfied =
        !observation.outputTargeted
        || observation.outputTargetSatisfied;
    const bool retainedHistoryRecoveryEligible =
        !observation.generatedWorkSample
        && observation.globalPressureValid
        && globalRecoveryHeadroom
        && !observation.outputDeficit
        && !observation.syntheticDropPressure;
    const bool recoveryTimingEligible =
        observation.generatedWorkSample || retainedHistoryRecoveryEligible;
    if (canRaise
            && !observation.deadlineMissed
            && recoveryTimingEligible
            && !observation.outputDeficit
            && !observation.syntheticDropPressure
            && !wsiPressure
            && outputRecoverySatisfied
            && globalRecoveryHeadroom) {
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
    case AdaptiveFlowDecisionReason::SustainedGlobalPressure: return "sustained_global_gpu_pressure";
    case AdaptiveFlowDecisionReason::EvaluatingDownstep: return "evaluating_downstep";
    case AdaptiveFlowDecisionReason::DownstepBenefitConfirmed: return "downstep_benefit_confirmed";
    case AdaptiveFlowDecisionReason::DownstepReverted: return "downstep_reverted_no_benefit";
    case AdaptiveFlowDecisionReason::InsufficientRecoveryHeadroom: return "insufficient_recovery_headroom";
    case AdaptiveFlowDecisionReason::SustainedHeadroom: return "sustained_headroom";
    }
    return "none";
}
