#include "adaptive_flow_controller.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>

namespace {
constexpr std::array<float, 7> kQualityStates{
    1.00F, 0.95F, 0.90F, 0.85F, 0.80F, 0.75F, 0.70F,
};
constexpr std::array<float, 6> kBalancedStates{
    0.80F, 0.75F, 0.70F, 0.65F, 0.60F, 0.55F,
};
constexpr std::array<float, 7> kLowStates{
    0.55F, 0.50F, 0.45F, 0.40F, 0.35F, 0.30F, 0.25F,
};
constexpr std::array<float, 16> kAutoStates{
    1.00F, 0.95F, 0.90F, 0.85F, 0.80F, 0.75F, 0.70F, 0.65F,
    0.60F, 0.55F, 0.50F, 0.45F, 0.40F, 0.35F, 0.30F, 0.25F,
};

constexpr double kPressureRatio = 0.90;
constexpr double kRecoveryPredictedRatio = 0.82;
constexpr double kMinimumFlowBudgetRatio = 0.10;
constexpr double kMinimumPredictedReliefRatio = 0.025;
constexpr double kMinimumGlobalPressureLsfgBudgetRatio = 0.40;
constexpr double kDownConfirmSeconds = 0.40;
constexpr double kGlobalDownConfirmSeconds = 0.20;
constexpr double kThermalAcceleratedConfirmSeconds = 0.25;
constexpr int kThermalStatusSevere = 3;
constexpr int kThermalStatusModerate = 2;
// Target/source pressure is user-visible; keep this loop comfortably faster
// than the previous confirmation/evaluation/cooldown chain.
constexpr double kOutputDownConfirmSeconds = 0.15;
constexpr double kUpConfirmSeconds = 1.75;
constexpr double kGlobalGpuPressurePercent = 96.0;
constexpr double kGlobalGpuRecoveryPercent = 88.0;
constexpr double kTransitionCooldownSeconds = 0.50;
constexpr double kOutputTransitionCooldownSeconds = 0.25;
// The backend's three-frame graph/history handoff is unchanged. This dwell
// only prevents stale pre-transition evidence from leaking past it.
constexpr double kFlowTransitionSettleSeconds = 0.25;
constexpr double kSchedulerTransitionHoldSeconds = 0.50;
constexpr double kDownstepEvaluationSeconds = 0.30;
constexpr double kOutputDownstepEvaluationSeconds = 0.15;
constexpr double kDownstepNoBenefitHoldSeconds = 2.0;
// Adaptive-FG quality upsteps are probes. Preserve source cadence rather than
// allowing a higher Flow state to force a new generated/source density regime.
constexpr double kRecoveryPacingEvaluationSeconds = 0.40;
constexpr double kRecoveryPacingRegressionConfirmSeconds = 0.15;
constexpr double kRecoveryPacingSourceRetentionRatio = 0.94;
constexpr double kRecoveryPacingDensityIncreaseTolerance = 0.25;
// A rejected Adaptive-FG quality state is a pacing cliff, not ordinary
// cooldown noise. Avoid periodic re-probing while the same scene is running.
constexpr double kRecoveryPacingRevertHoldSeconds = 15.0;
constexpr double kSourceTargetSatisfiedRatio = 0.98;
constexpr double kExploratorySourceDropRatio = 0.97;
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
    case AdaptiveFlowPreset::Auto: return kAutoStates;
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
    flowTransitionHoldActive_ = false;
    flowTransitionSettleUntilSeconds_ = 0.0;
    downstepEvaluationActive_ = false;
    downstepBenefitSeen_ = false;
    downstepOutputDriven_ = false;
    downstepSourceDriven_ = false;
    downstepExploratorySourceDriven_ = false;
    recoveryPacingEvaluationActive_ = false;
    recoveryPacingPreviousIndex_ = 0;
    recoveryPacingEvaluationStartedSeconds_ = 0.0;
    recoveryPacingRegressionSeconds_ = 0.0;
    recoveryPacingBaselineSourceFps_ = 0.0;
    recoveryPacingBaselineDensity_ = 0.0;
    resetFixedExploration();
    resetEvidence();
    selectTargetState();
    if (!enabled_)
        telemetry_.reason = AdaptiveFlowDecisionReason::Disabled;
}

bool AdaptiveFlowController::seedCurrentScale(float scale) {
    if (!enabled_ || !std::isfinite(scale))
        return false;

    const auto presetStates = states(preset_);
    const auto state = std::find_if(
        presetStates.begin(), presetStates.end(),
        [scale](float candidate) {
            return std::fabs(candidate - scale) <= 0.0005F;
        });
    if (state == presetStates.end())
        return false;

    reset();
    telemetry_.stateIndex = static_cast<std::size_t>(
        std::distance(presetStates.begin(), state));
    telemetry_.currentScale = *state;
    return true;
}

void AdaptiveFlowController::selectTargetState() {
    const auto presetStates = states(preset_);
    telemetry_ = {};
    telemetry_.targetScale = presetStates.front();
    telemetry_.minimumScale = presetStates.back();
    telemetry_.stateCount = presetStates.size();
    telemetry_.currentScale = presetStates.front();
    telemetry_.stateIndex = 0;
}

void AdaptiveFlowController::resetEvidence() {
    pressureSeconds_ = 0.0;
    headroomSeconds_ = 0.0;
}

void AdaptiveFlowController::resetFixedExploration() {
    fixedExplorationReferenceValid_ = false;
    fixedExplorationProbePending_ = false;
    fixedExplorationBestSourceFps_ = 0.0;
}

void AdaptiveFlowController::reset() {
    observedSeconds_ = 0.0;
    cooldownUntilSeconds_ = 0.0;
    schedulerHoldUntilSeconds_ = 0.0;
    flowTransitionHoldActive_ = false;
    flowTransitionSettleUntilSeconds_ = 0.0;
    downstepEvaluationActive_ = false;
    downstepBenefitSeen_ = false;
    downstepOutputDriven_ = false;
    downstepSourceDriven_ = false;
    downstepExploratorySourceDriven_ = false;
    recoveryPacingEvaluationActive_ = false;
    recoveryPacingPreviousIndex_ = 0;
    recoveryPacingEvaluationStartedSeconds_ = 0.0;
    recoveryPacingRegressionSeconds_ = 0.0;
    recoveryPacingBaselineSourceFps_ = 0.0;
    recoveryPacingBaselineDensity_ = 0.0;
    resetFixedExploration();
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
    telemetry_.thermalStatus = 0;
    telemetry_.thermalPressure = false;
    telemetry_.computePressure = false;
    telemetry_.wsiPressure = false;
    telemetry_.outputDeficit = false;
    telemetry_.outputPressure = false;
    telemetry_.sourcePressure = false;
    telemetry_.exploratorySourcePressure = false;
    telemetry_.sourceReferenceFps = 0.0;
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

    if (observation.flowTransition) {
        flowTransitionHoldActive_ = true;
        // The backend transition may finish before its output cadence has
        // re-established. Extend the barrier from the latest transition
        // sample so old pressure cannot immediately schedule another handoff.
        flowTransitionSettleUntilSeconds_ =
            observedSeconds_ + kFlowTransitionSettleSeconds;
    }
    if (flowTransitionHoldActive_) {
        if (downstepEvaluationActive_) {
            downstepEvaluationStartedSeconds_ = observedSeconds_;
            downstepBenefitSeen_ = false;
        }
        if (recoveryPacingEvaluationActive_) {
            recoveryPacingEvaluationStartedSeconds_ = observedSeconds_;
            recoveryPacingRegressionSeconds_ = 0.0;
        }
        if (!observation.flowTransition)
            flowTransitionHoldActive_ = false;
        resetEvidence();
        telemetry_.downstepEvaluationActive = downstepEvaluationActive_;
        telemetry_.reason = AdaptiveFlowDecisionReason::FlowTransition;
        return telemetry_.currentScale;
    }
    if (observedSeconds_ < flowTransitionSettleUntilSeconds_) {
        if (downstepEvaluationActive_) {
            downstepEvaluationStartedSeconds_ = observedSeconds_;
            downstepBenefitSeen_ = false;
        }
        if (recoveryPacingEvaluationActive_) {
            recoveryPacingEvaluationStartedSeconds_ = observedSeconds_;
            recoveryPacingRegressionSeconds_ = 0.0;
        }
        resetEvidence();
        telemetry_.downstepEvaluationActive = downstepEvaluationActive_;
        telemetry_.reason = AdaptiveFlowDecisionReason::FlowTransitionSettle;
        return telemetry_.currentScale;
    }

    if (!observation.valid
            || !(observation.frameBudgetMs > 0.0)
            || !std::isfinite(observation.frameBudgetMs)
            || observation.totalLsfgMs < 0.0
            || observation.flowMs < 0.0
            || !std::isfinite(observation.totalLsfgMs)
            || !std::isfinite(observation.flowMs)) {
        if (downstepEvaluationActive_)
            downstepEvaluationStartedSeconds_ += evidenceSeconds;
        if (recoveryPacingEvaluationActive_)
            recoveryPacingEvaluationStartedSeconds_ += evidenceSeconds;
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
    const bool freshGeneratedComputePressure =
        observation.generatedWorkSample
        && (observation.computeDeadlinePressure
            || observation.deadlineMissed
            || telemetry_.pressureRatio >= kPressureRatio);
    const bool retainedSevereComputePressure =
        observation.retainedGeneratedTimingSample
        && telemetry_.pressureRatio >= 1.0;
    const bool computePressure =
        freshGeneratedComputePressure || retainedSevereComputePressure;
    const bool wsiPressure = observation.wsiPresentationPressure;
    const bool fixedMultiplierMode =
        observation.fixedMultiplierMode
        || observation.fixedMultiplierBaseTarget;
    const bool sourceSampleValid =
        observation.sourceFps > 0.0 && std::isfinite(observation.sourceFps);
    const bool sourceTargetValid =
        fixedMultiplierMode
        && observation.sourceTargetFps > 0.0
        && std::isfinite(observation.sourceTargetFps);

    // A clean source target always supersedes exploratory state. If startup
    // never produced two trustworthy source-only intervals, use a separate
    // recovery reference to probe lower Flow states one at a time. A probe is
    // retained only when the source cadence measurably improves.
    if (!fixedMultiplierMode || sourceTargetValid) {
        resetFixedExploration();
    } else if (sourceSampleValid
            && observation.generatedWorkSample
            && observation.generationCount > 0) {
        if (!fixedExplorationReferenceValid_) {
            fixedExplorationReferenceValid_ = true;
            fixedExplorationBestSourceFps_ = observation.sourceFps;
            fixedExplorationProbePending_ = true;
        } else if (!downstepEvaluationActive_
                && observation.sourceFps > fixedExplorationBestSourceFps_) {
            fixedExplorationBestSourceFps_ = observation.sourceFps;
        }

        if (!fixedExplorationProbePending_
                && observation.sourceFps
                    < fixedExplorationBestSourceFps_
                        * kExploratorySourceDropRatio) {
            fixedExplorationProbePending_ = true;
        }
    }

    // Adaptive output targets are actionable. In Fixed mode the clean source
    // target governs Flow; a fixed output deficit alone may only reflect an
    // unreachable display cadence and must not back off the multiplier.
    const bool actionableOutputDeficit =
        observation.outputDeficit && !fixedMultiplierMode;
    const bool outputPressure =
        observation.outputCadenceValid
        && observation.outputTargeted
        && actionableOutputDeficit
        && !observation.outputTargetSatisfied;
    const bool sourcePressure =
        sourceTargetValid
        && sourceSampleValid
        && observation.sourceFps
            < observation.sourceTargetFps * kSourceTargetSatisfiedRatio;
    const bool exploratorySourcePressure =
        fixedMultiplierMode
        && !sourceTargetValid
        && fixedExplorationReferenceValid_
        && fixedExplorationProbePending_
        && sourceSampleValid
        && observation.generatedWorkSample
        && observation.generationCount > 0;
    const bool globalGpuPressure =
        observation.globalPressureValid
        && observation.globalGpuUsagePercent >= kGlobalGpuPressurePercent;
    const bool globalPressure =
        globalGpuPressure
        && (actionableOutputDeficit || observation.syntheticDropPressure);
    const bool thermalSevere =
        observation.thermalPressureValid
        && observation.thermalStatus >= kThermalStatusSevere;
    const bool wsiFlowPressure =
        wsiPressure && globalGpuPressure && actionableOutputDeficit;
    const bool recoveryWsiPressure =
        wsiPressure && !fixedMultiplierMode;
    telemetry_.computePressure = computePressure;
    telemetry_.wsiPressure = wsiPressure;
    telemetry_.globalPressure = globalPressure;
    telemetry_.thermalStatus =
        observation.thermalPressureValid ? observation.thermalStatus : 0;
    telemetry_.thermalPressure = thermalSevere;
    telemetry_.outputPressure = outputPressure;
    telemetry_.sourcePressure = sourcePressure;
    telemetry_.exploratorySourcePressure = exploratorySourcePressure;
    telemetry_.sourceReferenceFps = sourceTargetValid
        ? observation.sourceTargetFps
        : (fixedExplorationReferenceValid_
            ? fixedExplorationBestSourceFps_
            : 0.0);

    if (recoveryPacingEvaluationActive_) {
        const bool densitySampleValid =
            observation.adaptiveFramegenMode
            && observation.scheduledGenerationDensity >= 0.0
            && std::isfinite(observation.scheduledGenerationDensity);
        const bool sourceRegression =
            sourceSampleValid
            && recoveryPacingBaselineSourceFps_ > 0.0
            && observation.sourceFps
                < recoveryPacingBaselineSourceFps_
                    * kRecoveryPacingSourceRetentionRatio;
        const bool densityRegression =
            densitySampleValid
            && observation.scheduledGenerationDensity
                > recoveryPacingBaselineDensity_
                    + kRecoveryPacingDensityIncreaseTolerance;

        if (sourceRegression && densityRegression)
            recoveryPacingRegressionSeconds_ += evidenceSeconds;
        else
            recoveryPacingRegressionSeconds_ = 0.0;

        if (recoveryPacingRegressionSeconds_
                >= kRecoveryPacingRegressionConfirmSeconds) {
            const auto presetStates = states(preset_);
            telemetry_.stateIndex = std::min(
                recoveryPacingPreviousIndex_, presetStates.size() - 1);
            telemetry_.currentScale = presetStates[telemetry_.stateIndex];
            telemetry_.changed = true;
            telemetry_.reason =
                AdaptiveFlowDecisionReason::RecoveryPacingReverted;
            recoveryPacingEvaluationActive_ = false;
            recoveryPacingRegressionSeconds_ = 0.0;
            cooldownUntilSeconds_ =
                observedSeconds_ + kRecoveryPacingRevertHoldSeconds;
            resetEvidence();
            return telemetry_.currentScale;
        }

        if (observedSeconds_ - recoveryPacingEvaluationStartedSeconds_
                < kRecoveryPacingEvaluationSeconds) {
            resetEvidence();
            telemetry_.reason =
                AdaptiveFlowDecisionReason::EvaluatingRecoveryPacing;
            return telemetry_.currentScale;
        }

        recoveryPacingEvaluationActive_ = false;
        recoveryPacingRegressionSeconds_ = 0.0;
        telemetry_.reason =
            AdaptiveFlowDecisionReason::RecoveryPacingConfirmed;
        resetEvidence();
        return telemetry_.currentScale;
    }

    // Scheduler transitions suppress ordinary near-budget noise, but they
    // must not hide a completed generated batch that is already slower than
    // its entire LSFG budget. This is direct GPU timing, independent of the
    // potentially stale whole-device utilization sample.
    const bool severeTransitionComputePressure =
        computePressure && telemetry_.pressureRatio >= 1.0;

    if (observation.schedulerTransition) {
        if (downstepEvaluationActive_)
            downstepEvaluationStartedSeconds_ += evidenceSeconds;
        schedulerHoldUntilSeconds_ = std::max(
            schedulerHoldUntilSeconds_, observedSeconds_ + kSchedulerTransitionHoldSeconds);
        headroomSeconds_ = 0.0;
        if (!severeTransitionComputePressure) {
            if (globalPressure)
                pressureSeconds_ += evidenceSeconds;
            else
                pressureSeconds_ = 0.0;
            telemetry_.reason = AdaptiveFlowDecisionReason::SchedulerTransition;
            return telemetry_.currentScale;
        }
    }

    if (observedSeconds_ < schedulerHoldUntilSeconds_
            && !severeTransitionComputePressure) {
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
        // A target-driven step is useful while its measured target remains
        // missed. Reverting solely because one short, noisy cadence window did
        // not improve enough caused adjacent-state oscillation without ever
        // recovering the requested rate.
        const bool directOutputTargetMiss =
            !fixedMultiplierMode
            && observation.outputCadenceValid
            && observation.outputTargeted
            && !observation.outputTargetSatisfied;
        const bool targetPressureRemains =
            (downstepOutputDriven_
                && directOutputTargetMiss
                && !downstepBaselineWsiPressure_)
            || (downstepSourceDriven_ && sourcePressure);

        if (downstepExploratorySourceDriven_) {
            // Missing-baseline probes must recover source cadence. Cheaper GPU
            // work alone is not enough to justify a quality reduction.
            downstepBenefitSeen_ = downstepBenefitSeen_ || sourceBenefit;
        } else {
            downstepBenefitSeen_ = downstepBenefitSeen_
                || outputBenefit || sourceBenefit || computeBenefit
                || wsiBenefit || globalBenefit || targetPressureRemains;
        }

        const double downstepEvaluationSeconds =
            (downstepOutputDriven_
                || downstepSourceDriven_
                || downstepExploratorySourceDriven_)
                ? kOutputDownstepEvaluationSeconds
                : kDownstepEvaluationSeconds;
        if (observedSeconds_ - downstepEvaluationStartedSeconds_
                < downstepEvaluationSeconds) {
            resetEvidence();
            telemetry_.downstepEvaluationActive = true;
            telemetry_.reason = AdaptiveFlowDecisionReason::EvaluatingDownstep;
            return telemetry_.currentScale;
        }

        downstepEvaluationActive_ = false;
        telemetry_.downstepEvaluationActive = false;
        resetEvidence();
        if (downstepBenefitSeen_) {
            if (downstepExploratorySourceDriven_) {
                fixedExplorationReferenceValid_ = true;
                fixedExplorationBestSourceFps_ = std::max(
                    fixedExplorationBestSourceFps_, observation.sourceFps);
                const auto presetStates = states(preset_);
                fixedExplorationProbePending_ =
                    telemetry_.stateIndex + 1 < presetStates.size();
            }
            downstepBenefitSeen_ = false;
            downstepOutputDriven_ = false;
            downstepSourceDriven_ = false;
            downstepExploratorySourceDriven_ = false;
            telemetry_.reason =
                AdaptiveFlowDecisionReason::DownstepBenefitConfirmed;
            return telemetry_.currentScale;
        }

        const auto presetStates = states(preset_);
        telemetry_.stateIndex = std::min(
            downstepPreviousIndex_, presetStates.size() - 1);
        telemetry_.currentScale = presetStates[telemetry_.stateIndex];
        telemetry_.changed = true;
        if (downstepExploratorySourceDriven_)
            fixedExplorationProbePending_ = false;
        downstepOutputDriven_ = false;
        downstepSourceDriven_ = false;
        downstepExploratorySourceDriven_ = false;
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
        computePressure || globalPressure || wsiFlowPressure
        || outputPressure || sourcePressure || exploratorySourcePressure;
    const bool targetPressure = outputPressure || sourcePressure;
    const bool fastPressure = targetPressure || exploratorySourcePressure;
    // Thermal status is never an actuator by itself. It only shortens the
    // confirmation interval after an existing LSFG/source/output pressure
    // signal has already made Flow a legitimate actuator.
    const bool thermalAcceleratedPressure =
        thermalSevere && pressure && !fastPressure && !globalPressure;

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
        // A trustworthy source/output miss remains authoritative all the way
        // to the preset floor. Do not stop early because one 0.05 Flow step
        // looks small in isolation. Missing-baseline exploration retains the
        // material-contribution probe gate.
        const bool targetDrivenPressure = targetPressure && !computePressure;
        const bool exploratoryDrivenPressure =
            exploratorySourcePressure && !targetPressure && !computePressure;
        const bool materiallyUsefulForExploration =
            observation.flowMs >= 1.0 && predictedReliefMs >= 0.25;
        const bool globalOnlyPressure =
            globalPressure && !computePressure && !fastPressure;
        const bool globallyProfitable =
            !globalOnlyPressure
            || telemetry_.pressureRatio >= kMinimumGlobalPressureLsfgBudgetRatio;
        if ((!targetDrivenPressure
                && ((exploratoryDrivenPressure
                        && !materiallyUsefulForExploration)
                    || (!fastPressure
                        && (telemetry_.flowBudgetRatio < kMinimumFlowBudgetRatio
                            || predictedReliefRatio < kMinimumPredictedReliefRatio))))
                || !globallyProfitable) {
            resetEvidence();
            telemetry_.reason = AdaptiveFlowDecisionReason::InsufficientFlowContribution;
            return telemetry_.currentScale;
        }

        pressureSeconds_ += evidenceSeconds;
        headroomSeconds_ = 0.0;
        const double downConfirmSeconds =
            fastPressure
                ? kOutputDownConfirmSeconds
                : (globalPressure
                    ? kGlobalDownConfirmSeconds
                    : (thermalAcceleratedPressure
                        ? kThermalAcceleratedConfirmSeconds
                        : kDownConfirmSeconds));
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
            downstepOutputDriven_ = outputPressure;
            downstepSourceDriven_ = sourcePressure;
            downstepExploratorySourceDriven_ =
                exploratorySourcePressure && !targetPressure;
            if (downstepExploratorySourceDriven_)
                fixedExplorationProbePending_ = false;
            // Fast pressure shortens confirmation, but every actuator change
            // remains one discrete state. Skipping a state makes the benefit
            // evaluator compare two unmeasured graph changes at once and was
            // a direct source of Adaptive Flow oscillation.
            constexpr std::size_t pressureStepCount = 1U;
            telemetry_.stateIndex = std::min(
                index + pressureStepCount, presetStates.size() - 1);
            telemetry_.currentScale = presetStates[telemetry_.stateIndex];
            telemetry_.changed = true;
            telemetry_.downstepEvaluationActive = true;
            telemetry_.reason = outputPressure
                ? AdaptiveFlowDecisionReason::SustainedOutputPressure
                : (sourcePressure
                    ? AdaptiveFlowDecisionReason::SustainedSourcePressure
                    : (downstepExploratorySourceDriven_
                        ? AdaptiveFlowDecisionReason::ExploratorySourcePressure
                        : (globalPressure
                            ? AdaptiveFlowDecisionReason::SustainedGlobalPressure
                            : AdaptiveFlowDecisionReason::SustainedPressure)));
            cooldownUntilSeconds_ = observedSeconds_
                + (fastPressure
                    ? kOutputTransitionCooldownSeconds
                    : kTransitionCooldownSeconds);
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
    const bool thermalRecoveryHeadroom =
        !observation.thermalPressureValid
        || observation.thermalStatus <= kThermalStatusModerate;
    const bool outputRecoverySatisfied =
        fixedMultiplierMode
        || !observation.outputTargeted
        || observation.outputTargetSatisfied;
    // Fixed mode commonly starts with generated work already active, so the
    // clean source-only tracker can legitimately have no baseline. Once the
    // exploratory hill-climb has settled (no probe pending), treat its best
    // measured source cadence as the recovery reference. This lets quality
    // rise again when the scene becomes cheaper without inventing a target.
    const bool exploratorySourceRecoverySatisfied =
        fixedMultiplierMode
        && !sourceTargetValid
        && fixedExplorationReferenceValid_
        && !fixedExplorationProbePending_
        && sourceSampleValid
        && observation.sourceFps
            >= fixedExplorationBestSourceFps_
                * kExploratorySourceDropRatio;
    const bool sourceRecoverySatisfied =
        !fixedMultiplierMode
        || (sourceTargetValid
            && observation.sourceFps >= observation.sourceTargetFps
                * kSourceTargetSatisfiedRatio)
        || exploratorySourceRecoverySatisfied;
    const bool retainedHistoryRecoveryEligible =
        !observation.generatedWorkSample
        && observation.globalPressureValid
        && globalRecoveryHeadroom
        && !actionableOutputDeficit
        && !observation.syntheticDropPressure;
    const bool recoveryTimingEligible =
        observation.generatedWorkSample || retainedHistoryRecoveryEligible;
    if (canRaise
            && !observation.deadlineMissed
            && recoveryTimingEligible
            && !actionableOutputDeficit
            && !observation.syntheticDropPressure
            && !recoveryWsiPressure
            && outputRecoverySatisfied
            && sourceRecoverySatisfied
            && globalRecoveryHeadroom
            && thermalRecoveryHeadroom) {
        const double currentScale = static_cast<double>(presetStates[index]);
        // Recover one .05 state per confirmed headroom decision. The newly
        // selected state must produce fresh timing before another upstep.
        constexpr std::size_t recoveryStepCount = 1U;
        const std::size_t recoveryIndex = index - recoveryStepCount;
        const double higherScale = static_cast<double>(presetStates[recoveryIndex]);
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
            telemetry_.stateIndex = recoveryIndex;
            telemetry_.currentScale = presetStates[telemetry_.stateIndex];
            telemetry_.changed = true;
            telemetry_.reason = AdaptiveFlowDecisionReason::SustainedHeadroom;
            if (observation.adaptiveFramegenMode
                    && sourceSampleValid
                    && observation.scheduledGenerationDensity >= 0.0
                    && std::isfinite(observation.scheduledGenerationDensity)) {
                recoveryPacingEvaluationActive_ = true;
                recoveryPacingPreviousIndex_ = index;
                recoveryPacingEvaluationStartedSeconds_ = observedSeconds_;
                recoveryPacingRegressionSeconds_ = 0.0;
                recoveryPacingBaselineSourceFps_ = observation.sourceFps;
                recoveryPacingBaselineDensity_ =
                    observation.scheduledGenerationDensity;
            }
            if (fixedMultiplierMode && !sourceTargetValid) {
                // An upward quality probe must not immediately re-arm the
                // downward exploratory probe. Re-arm only if source cadence
                // actually degrades below the best learned reference.
                fixedExplorationProbePending_ = false;
            }
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

std::optional<float> AdaptiveFlowController::conservativeSeedScale(
        AdaptiveFlowPreset preset, float scale) {
    if (!std::isfinite(scale))
        return std::nullopt;

    const auto presetStates = states(preset);
    constexpr float kScaleEpsilon = 0.0005F;
    for (const float candidate : presetStates) {
        if (candidate <= scale + kScaleEpsilon)
            return candidate;
    }
    // The configured fixed scale may sit below a preset's quality floor.
    // In that case the preset floor is the least expensive legal state.
    return presetStates.back();
}

const char* AdaptiveFlowController::presetName(AdaptiveFlowPreset preset) {
    switch (preset) {
    case AdaptiveFlowPreset::Quality: return "quality";
    case AdaptiveFlowPreset::Balanced: return "balanced";
    case AdaptiveFlowPreset::Low: return "low";
    case AdaptiveFlowPreset::Auto: return "auto";
    }
    return "quality";
}

const char* AdaptiveFlowController::reasonName(AdaptiveFlowDecisionReason reason) {
    switch (reason) {
    case AdaptiveFlowDecisionReason::None: return "none";
    case AdaptiveFlowDecisionReason::Disabled: return "disabled";
    case AdaptiveFlowDecisionReason::InvalidTelemetry: return "invalid_telemetry";
    case AdaptiveFlowDecisionReason::FlowTransition: return "flow_transition";
    case AdaptiveFlowDecisionReason::FlowTransitionSettle:
        return "flow_transition_settle";
    case AdaptiveFlowDecisionReason::SchedulerTransition: return "adaptive_lsfg_transition";
    case AdaptiveFlowDecisionReason::Cooldown: return "cooldown";
    case AdaptiveFlowDecisionReason::InsufficientFlowContribution: return "insufficient_flow_contribution";
    case AdaptiveFlowDecisionReason::SustainedPressure: return "sustained_gpu_pressure";
    case AdaptiveFlowDecisionReason::SustainedGlobalPressure: return "sustained_global_gpu_pressure";
    case AdaptiveFlowDecisionReason::SustainedOutputPressure: return "sustained_output_pressure";
    case AdaptiveFlowDecisionReason::SustainedSourcePressure: return "sustained_source_pressure";
    case AdaptiveFlowDecisionReason::ExploratorySourcePressure: return "exploratory_source_pressure";
    case AdaptiveFlowDecisionReason::EvaluatingDownstep: return "evaluating_downstep";
    case AdaptiveFlowDecisionReason::DownstepBenefitConfirmed: return "downstep_benefit_confirmed";
    case AdaptiveFlowDecisionReason::DownstepReverted: return "downstep_reverted_no_benefit";
    case AdaptiveFlowDecisionReason::InsufficientRecoveryHeadroom: return "insufficient_recovery_headroom";
    case AdaptiveFlowDecisionReason::SustainedHeadroom: return "sustained_headroom";
    case AdaptiveFlowDecisionReason::EvaluatingRecoveryPacing:
        return "evaluating_recovery_pacing";
    case AdaptiveFlowDecisionReason::RecoveryPacingConfirmed:
        return "recovery_pacing_confirmed";
    case AdaptiveFlowDecisionReason::RecoveryPacingReverted:
        return "recovery_pacing_reverted";
    }
    return "none";
}
