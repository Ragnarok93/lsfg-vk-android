#include "adaptive_scheduler.hpp"

#include <algorithm>
#include <cmath>

namespace {
constexpr double kSlowdownCadenceAlpha = 0.45;
constexpr double kSpeedupCadenceAlpha = 0.30;
constexpr double kCadenceTargetMinRatio = 0.75;
constexpr double kCadenceTargetMaxRatio = 1.30;
constexpr double kOpportunityIntervalMaxRatio = 1.50;
constexpr unsigned kCapacityRaiseSamplesRequired = 4;
constexpr std::size_t kCapacityCadenceWindow = 4;
constexpr double kCapacityCadenceDeviationRatio = 0.20;
// Treat a single interval as a suspend/stall discontinuity only when it is an
// extreme outlier relative to an already-established source cadence. An
// absolute FPS threshold would incorrectly disable generation for legitimately
// slow sources.
constexpr double kDiscontinuityRatio = 8.0;
constexpr uint64_t kSourceTimelineDiscontinuityRatio = 8ULL;

// Debounce brief demand spikes without turning source degradation into a
// generation-count veto. Adaptive continues pursuing the configured output
// target up to maxGeneratedFrames_ even when the source slows under load.
constexpr double kSustainedDemandSeconds = 0.600;

// Generation density is a cadence decision, not a raw frame-time reaction.
// Acquire an integer-ratio regime quickly once smoothed demand is close, then
// require wider and sustained evidence before leaving it.
constexpr double kIntegerDensityAcquireWindow = 0.10;
constexpr double kIntegerDensityReleaseWindow = 0.20;
constexpr unsigned kIntegerDensityAcquireSamples = 4;
constexpr unsigned kIntegerDensityReleaseSamples = 8;
} // namespace

SourceTimelineSample SourceProtectedTimeline::observe(
        uint64_t sourceArrivalTimeNs,
        std::chrono::nanoseconds sourceInterval,
        bool discontinuity) {
    SourceTimelineSample sample{};
    const auto intervalCount = sourceInterval.count();
    if (sourceArrivalTimeNs == 0 || intervalCount <= 0)
        return sample;

    const uint64_t intervalNs = static_cast<uint64_t>(intervalCount);
    const uint64_t discontinuityThreshold =
        lastIntervalNs_ > std::numeric_limits<uint64_t>::max()
            / kSourceTimelineDiscontinuityRatio
            ? std::numeric_limits<uint64_t>::max()
            : lastIntervalNs_ * kSourceTimelineDiscontinuityRatio;
    if (discontinuity
            || (initialized_
                && lastIntervalNs_ > 0
                && intervalNs > discontinuityThreshold)) {
        reset();
        return sample;
    }

    const auto addSaturated = [](uint64_t base, uint64_t delta) {
        return base > std::numeric_limits<uint64_t>::max() - delta
            ? std::numeric_limits<uint64_t>::max()
            : base + delta;
    };

    if (!initialized_) {
        initialized_ = true;
        sourceIndex_ = 0;
        predictedIntervalNs_ = intervalNs;
        sample.previousSourceDesiredTimeNs = sourceArrivalTimeNs;
        sourceDesiredTimeNs_ =
            addSaturated(sourceArrivalTimeNs, predictedIntervalNs_);
        sample.rebased = true;
        sample.sourceDeadlineErrorNs = 0;
    } else {
        ++sourceIndex_;

        // Measure the phase error against the prediction made by the previous
        // real source frame. Generated success/failure never feeds this clock.
        const uint64_t predictedArrivalTimeNs = sourceDesiredTimeNs_;
        uint64_t absoluteErrorNs = 0;
        if (sourceArrivalTimeNs >= predictedArrivalTimeNs) {
            const uint64_t delta = sourceArrivalTimeNs - predictedArrivalTimeNs;
            absoluteErrorNs = delta;
            sample.sourceDeadlineErrorNs = delta
                > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
                ? std::numeric_limits<int64_t>::max()
                : static_cast<int64_t>(delta);
        } else {
            const uint64_t delta = predictedArrivalTimeNs - sourceArrivalTimeNs;
            absoluteErrorNs = delta;
            sample.sourceDeadlineErrorNs = delta
                > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
                ? std::numeric_limits<int64_t>::min()
                : -static_cast<int64_t>(delta);
        }

        constexpr uint64_t kMinPhaseCorrectionNs = 1'000'000ULL;
        constexpr uint64_t kMaxPhaseCorrectionNs = 8'000'000ULL;
        const uint64_t materialPhaseErrorNs = std::clamp<uint64_t>(
            predictedIntervalNs_ / 4ULL,
            kMinPhaseCorrectionNs,
            kMaxPhaseCorrectionNs);
        sample.rebased = absoluteErrorNs >= materialPhaseErrorNs;

        // Real source arrival is the phase anchor, but raw frame time is not the
        // next generation budget. Increase the prediction slowly under a
        // sustained slowdown so one late source cannot grant a huge synthetic
        // window, and contract quickly when the source speeds up so generation
        // cannot steal time from a sooner next source.
        const long double predicted =
            static_cast<long double>(predictedIntervalNs_);
        const long double observed = static_cast<long double>(intervalNs);
        const long double boundedObserved = std::clamp(
            observed,
            predicted * 0.50L,
            predicted * 1.50L);
        const long double alpha =
            boundedObserved < predicted ? 0.50L : 0.20L;
        const long double updated =
            predicted + alpha * (boundedObserved - predicted);
        predictedIntervalNs_ = std::max<uint64_t>(
            1ULL, static_cast<uint64_t>(std::llround(updated)));

        sample.previousSourceDesiredTimeNs = sourceArrivalTimeNs;
        sourceDesiredTimeNs_ =
            addSaturated(sourceArrivalTimeNs, predictedIntervalNs_);
    }

    lastIntervalNs_ = intervalNs;
    sample.sourceIndex = sourceIndex_;
    sample.intervalNs = predictedIntervalNs_;
    sample.sourceDesiredTimeNs = sourceDesiredTimeNs_;
    sample.valid = true;
    return sample;
}

uint64_t SourceProtectedTimeline::syntheticDesiredTimeNs(
        const SourceTimelineSample& sample, double interpolationFraction) const {
    if (!sample.valid
            || !(interpolationFraction > 0.0)
            || !(interpolationFraction < 1.0)
            || sample.sourceDesiredTimeNs <= sample.previousSourceDesiredTimeNs)
        return 0;

    const long double span = static_cast<long double>(
        sample.sourceDesiredTimeNs - sample.previousSourceDesiredTimeNs);
    const long double offset = span * static_cast<long double>(interpolationFraction);
    const uint64_t offsetNs = static_cast<uint64_t>(offset);
    return sample.previousSourceDesiredTimeNs + offsetNs;
}

void SourceProtectedTimeline::reset() {
    initialized_ = false;
    sourceIndex_ = 0;
    sourceDesiredTimeNs_ = 0;
    lastIntervalNs_ = 0;
    predictedIntervalNs_ = 0;
}

const char* sourceCadenceObservationName(
        SourceCadenceObservation observation) {
    switch (observation) {
    case SourceCadenceObservation::SourceOnly:
        return "source-only";
    case SourceCadenceObservation::HistoryMaintenance:
        return "history-maintenance";
    case SourceCadenceObservation::Generated:
        return "generated";
    }
    return "unknown";
}

double framegenBatchBudgetMs(
        double sourceIntervalMs,
        double nominalBatchBudgetMs,
        bool fullSourceIntervalBudget) {
    const bool sourceIntervalValid =
        sourceIntervalMs > 0.0 && std::isfinite(sourceIntervalMs);
    const bool nominalValid =
        nominalBatchBudgetMs > 0.0 && std::isfinite(nominalBatchBudgetMs);

    if (fullSourceIntervalBudget && sourceIntervalValid)
        return sourceIntervalMs;
    return nominalValid ? nominalBatchBudgetMs : 0.0;
}

void DeadlineAdmissionPredictor::observe(
        const DeadlineAdmissionObservation& observation) {
    if (!observation.valid
            || observation.generationCount == 0
            || observation.generationCount >= kTrackedBatchCounts
            || !std::isfinite(observation.mipmapsMs)
            || !std::isfinite(observation.opticalFlowMs)
            || !std::isfinite(observation.totalLsfgMs)
            || observation.mipmapsMs < 0.0
            || observation.opticalFlowMs < 0.0
            || observation.totalLsfgMs <= 0.0) {
        return;
    }

    auto& estimate = batchEstimates_.at(observation.generationCount);
    const auto conservativeBlend = [&](double current, double sample) {
        if (!estimate.valid || sample >= current)
            return sample;
        return current + kRecoveryEwmaAlpha * (sample - current);
    };
    estimate.mipmapsMs =
        conservativeBlend(estimate.mipmapsMs, observation.mipmapsMs);
    estimate.opticalFlowMs =
        conservativeBlend(estimate.opticalFlowMs, observation.opticalFlowMs);
    const bool hadGpuEstimate =
        std::isfinite(estimate.gpuTotalLsfgMs)
        && estimate.gpuTotalLsfgMs > 0.0;
    if (!hadGpuEstimate || observation.totalLsfgMs >= estimate.gpuTotalLsfgMs) {
        estimate.gpuTotalLsfgMs = observation.totalLsfgMs;
    } else {
        estimate.gpuTotalLsfgMs +=
            kRecoveryEwmaAlpha
            * (observation.totalLsfgMs - estimate.gpuTotalLsfgMs);
    }
    if (!estimate.blockingCompletionObserved) {
        estimate.totalLsfgMs =
            conservativeBlend(estimate.totalLsfgMs, observation.totalLsfgMs);
    } else if (!estimate.valid
            || observation.totalLsfgMs > estimate.totalLsfgMs) {
        // Once the protected source thread has measured a blocking completion,
        // the same batch's GPU timestamp cannot be treated as recovery evidence.
        // Only a later blocking completion or a clean source-only recovery cycle
        // may relax that source-owned queue-residency penalty.
        estimate.totalLsfgMs = observation.totalLsfgMs;
    }
    estimate.valid = true;
    hasEstimate_ = true;
}

void DeadlineAdmissionPredictor::observeBlockingCompletion(
        std::size_t generationCount, double completionMs) {
    if (generationCount == 0
            || generationCount >= kTrackedBatchCounts
            || !std::isfinite(completionMs)
            || completionMs <= 0.0) {
        return;
    }

    auto& estimate = batchEstimates_.at(generationCount);
    estimate.blockingCompletionObserved = true;
    if (!estimate.valid || completionMs >= estimate.totalLsfgMs) {
        estimate.totalLsfgMs = completionMs;
    } else {
        // A saturated queue must close admission immediately, while recovery
        // requires repeated fast completion evidence. This is the same
        // conservative decay used for lower GPU timing samples.
        estimate.totalLsfgMs +=
            kRecoveryEwmaAlpha * (completionMs - estimate.totalLsfgMs);
    }
    estimate.valid = true;
    hasEstimate_ = true;
}

void DeadlineAdmissionPredictor::observeSourceOnlyRecovery() {
    for (auto& estimate : batchEstimates_) {
        if (!estimate.valid
                || !estimate.blockingCompletionObserved
                || !std::isfinite(estimate.gpuTotalLsfgMs)
                || !(estimate.gpuTotalLsfgMs > 0.0)
                || !std::isfinite(estimate.totalLsfgMs)
                || estimate.totalLsfgMs <= estimate.gpuTotalLsfgMs) {
            continue;
        }

        estimate.totalLsfgMs +=
            kRecoveryEwmaAlpha
            * (estimate.gpuTotalLsfgMs - estimate.totalLsfgMs);
        estimate.totalLsfgMs = std::max(
            estimate.totalLsfgMs, estimate.gpuTotalLsfgMs);
    }
}

void DeadlineAdmissionPredictor::observeDeliveryMiss(double latenessMs) {
    if (!std::isfinite(latenessMs) || latenessMs < 0.0)
        return;

    const double sample = std::clamp(
        std::max(latenessMs, kDeliveryReserveFloorMs),
        kDeliveryReserveFloorMs,
        kDeliveryReserveMaxMs);
    deliveryReserveMs_ = deliveryReserveMs_ > 0.0
        ? deliveryReserveMs_
            + kDeliveryReserveAlpha * (sample - deliveryReserveMs_)
        : sample;
}

void DeadlineAdmissionPredictor::observeDeliverySuccess() {
    deliveryReserveMs_ *= kDeliveryReserveSuccessDecay;
    if (deliveryReserveMs_ < 0.01)
        deliveryReserveMs_ = 0.0;
}

DeadlineAdmissionDecision DeadlineAdmissionPredictor::predict(
        std::size_t generationCount, double usableBudgetMs) const {
    DeadlineAdmissionDecision decision{
        .usableBudgetMs = usableBudgetMs,
        .deliveryReserveMs = deliveryReserveMs_,
        .effectiveUsableBudgetMs = std::max(
            0.0, usableBudgetMs - deliveryReserveMs_),
    };
    if (!hasEstimate_
            || generationCount == 0
            || generationCount >= kTrackedBatchCounts
            || !(usableBudgetMs > 0.0)
            || !std::isfinite(usableBudgetMs)) {
        return decision;
    }

    const BatchCostEstimate* estimate = nullptr;
    std::size_t estimateCount = 0;
    if (batchEstimates_.at(generationCount).valid) {
        estimate = &batchEstimates_.at(generationCount);
        estimateCount = generationCount;
    } else {
        // Prefer the nearest measured whole-batch count. The fallback scales
        // the complete batch cost with an extra guard instead of pretending
        // mipmaps/flow are perfectly shared and generation cost is linear.
        for (std::size_t distance = 1;
                distance < kTrackedBatchCounts && estimate == nullptr;
                ++distance) {
            if (generationCount > distance) {
                const std::size_t lower = generationCount - distance;
                if (batchEstimates_.at(lower).valid) {
                    estimate = &batchEstimates_.at(lower);
                    estimateCount = lower;
                    break;
                }
            }
            const std::size_t higher = generationCount + distance;
            if (higher < kTrackedBatchCounts
                    && batchEstimates_.at(higher).valid) {
                estimate = &batchEstimates_.at(higher);
                estimateCount = higher;
            }
        }
    }
    if (estimate == nullptr || estimateCount == 0)
        return decision;

    const bool exact = estimateCount == generationCount;
    const double relativeCount =
        static_cast<double>(generationCount)
        / static_cast<double>(estimateCount);
    const double scale = exact
        ? 1.0
        : (generationCount < estimateCount
            ? std::max(0.70, relativeCount * kUnknownBatchSafetyRatio)
            : relativeCount * kUnknownBatchSafetyRatio);
    decision.predictedMipmapsMs = estimate->mipmapsMs;
    decision.predictedOpticalFlowMs = estimate->opticalFlowMs;
    decision.predictedTotalLsfgMs = estimate->totalLsfgMs * scale;
    decision.safetyMarginMs = std::max(
        kSafetyMarginFloorMs,
        decision.predictedTotalLsfgMs * kSafetyMarginRatio);
    decision.valid = std::isfinite(decision.predictedTotalLsfgMs)
        && decision.predictedTotalLsfgMs > 0.0;
    decision.wouldAdmit = decision.valid
        && decision.predictedTotalLsfgMs + decision.safetyMarginMs
            <= decision.effectiveUsableBudgetMs;
    return decision;
}

std::size_t DeadlineAdmissionPredictor::safeGenerationHint(
        std::size_t maxGenerationCount, double sourceIntervalMs) const {
    if (!hasEstimate_
            || maxGenerationCount == 0
            || !(sourceIntervalMs > 0.0)
            || !std::isfinite(sourceIntervalMs)) {
        return 0;
    }

    // Capacity promotion is intentionally conservative. A candidate is safe
    // only when every prefix fits the evenly-spaced slot it would actually own.
    for (std::size_t candidate = maxGenerationCount; candidate > 0; --candidate) {
        bool fits = true;
        for (std::size_t slot = 0; slot < candidate; ++slot) {
            const double slotBudgetMs = sourceIntervalMs
                * static_cast<double>(slot + 1)
                / static_cast<double>(candidate + 1);
            const auto decision = predict(slot + 1, slotBudgetMs);
            if (!decision.valid || !decision.wouldAdmit) {
                fits = false;
                break;
            }
        }
        if (fits)
            return candidate;
    }
    return 0;
}

std::size_t DeadlineAdmissionPredictor::safeBatchGenerationHint(
        std::size_t maxGenerationCount,
        double batchBudgetMs) const {
    if (!hasEstimate_
            || maxGenerationCount == 0
            || !(batchBudgetMs > 0.0)
            || !std::isfinite(batchBudgetMs)) {
        return 0;
    }

    for (std::size_t candidate = maxGenerationCount;
            candidate > 0; --candidate) {
        const auto decision = predict(
            candidate, batchBudgetMs);
        if (decision.valid && decision.wouldAdmit)
            return candidate;
    }
    return 0;
}

void DeadlineAdmissionPredictor::reset() {
    hasEstimate_ = false;
    batchEstimates_ = {};
    deliveryReserveMs_ = 0.0;
}

namespace {
constexpr unsigned kWsiEvidenceThreshold = 5;
constexpr unsigned kWsiEvidenceIncrement = 2;
constexpr unsigned kWsiEvidenceDecay = 1;
constexpr double kWsiPressureRatio = 0.20;
constexpr double kWsiSevereSingleFramePressureRatio = 0.50;
constexpr double kWsiAcceptanceAlpha = 0.25;
constexpr double kWsiRecoveryGoodEfficiency = 0.80;
constexpr double kWsiRecoveryEvidenceIncrement = 1.0;
constexpr double kWsiRecoveryEvidenceRejectDecay = 0.25;
constexpr double kWsiRecoveryProbeThreshold = 8.0;
constexpr double kWsiDeficitProbeThreshold = 2.5;
constexpr unsigned kWsiProvisionalEvaluationSamples = 6;
constexpr double kWsiEfficiencyImprovement = 0.08;
constexpr double kWsiThroughputPreserveRatio = 0.95;
constexpr std::array<double, 4> kSingleFrameDuties{
    1.0, 0.75, 0.50, 0.25,
};

double nextLowerDuty(double duty) {
    for (std::size_t index = 0; index + 1 < kSingleFrameDuties.size(); ++index) {
        if (duty >= kSingleFrameDuties[index] - 1e-6)
            return kSingleFrameDuties[index + 1];
    }
    return kSingleFrameDuties.back();
}

double nextHigherDuty(double duty) {
    for (std::size_t index = kSingleFrameDuties.size() - 1; index > 0; --index) {
        if (duty <= kSingleFrameDuties[index] + 1e-6)
            return kSingleFrameDuties[index - 1];
    }
    return kSingleFrameDuties.front();
}

bool higherCapacityIsProven(
        const GeneratedPresentationCapacityContext& context,
        std::size_t currentCap) {
    constexpr int64_t kMaximumSourceDeadlineEvidenceNs = 8'000'000;
    return context.deadlineCapacityValid
        && context.safeGenerationHint > currentCap
        && context.schedulerCostLimit > currentCap
        && context.sourceInsideBudget
        && context.sourceDeadlineErrorNs <= kMaximumSourceDeadlineEvidenceNs
        && context.higherCapacityProven;
}
} // namespace

const char* generatedPresentationCapChangeReasonName(
        GeneratedPresentationCapChangeReason reason) {
    switch (reason) {
        case GeneratedPresentationCapChangeReason::RejectionProbe:
            return "rejection_probe";
        case GeneratedPresentationCapChangeReason::ProfitabilityKeepLower:
            return "profitability_keep_lower";
        case GeneratedPresentationCapChangeReason::ProfitabilityRestoreHigher:
            return "profitability_restore_higher";
        case GeneratedPresentationCapChangeReason::RecoveryEvidenceRaise:
            return "recovery_evidence_raise";
        case GeneratedPresentationCapChangeReason::TargetDeficitProbeSuccess:
            return "target_deficit_probe_success";
        case GeneratedPresentationCapChangeReason::SubOneDutyLower:
            return "sub_one_duty_lower";
        case GeneratedPresentationCapChangeReason::SubOneDutyRecover:
            return "sub_one_duty_recover";
        case GeneratedPresentationCapChangeReason::None:
        default:
            return "none";
    }
}

void GeneratedPresentationCapacityTracker::configure(
        std::size_t maxGeneratedFrames) {
    if (maxGeneratedFrames_ == maxGeneratedFrames)
        return;

    maxGeneratedFrames_ = maxGeneratedFrames;
    rejectionEvidence_ = 0;
    recoveryEvidence_ = 0.0;
    singleFramePhase_ = 0.0;
    hasObservation_ = false;
    acceptedFramesEwma_ = 0.0;
    efficiencyEwma_ = 0.0;
    provisionalLowerActive_ = false;
    provisionalPreviousCap_ = 0;
    provisionalSamples_ = 0;
    provisionalAcceptedSum_ = 0.0;
    provisionalEfficiencySum_ = 0.0;
    provisionalBaselineAccepted_ = 0.0;
    provisionalBaselineEfficiency_ = 0.0;
    upwardProbePending_ = false;
    upwardProbeInFlight_ = false;
    upwardProbeAttempted_ = 0;
    telemetry_ = {};
    telemetry_.generationCap = maxGeneratedFrames_;
    telemetry_.singleFrameDuty = 1.0;
}

std::size_t GeneratedPresentationCapacityTracker::limit(
        std::size_t requested) {
    return limit(requested, GeneratedPresentationCapacityContext{});
}

std::size_t GeneratedPresentationCapacityTracker::limit(
        std::size_t requested,
        const GeneratedPresentationCapacityContext& context) {
    if (requested == 0 || maxGeneratedFrames_ == 0)
        return 0;

    // A probe is valid only when it reaches observation in the same cycle. A
    // later deadline drop must not be credited as a successful capacity test.
    if (upwardProbeInFlight_) {
        upwardProbeInFlight_ = false;
        upwardProbeAttempted_ = 0;
    }

    const bool provenHigherCapacity = higherCapacityIsProven(
        context, telemetry_.generationCap);
    const bool deficitProbeEligible =
        context.outputDeficit && provenHigherCapacity;
    const bool genericRecoveryProbe =
        recoveryEvidence_ >= kWsiRecoveryProbeThreshold
        && efficiencyEwma_ >= kWsiRecoveryGoodEfficiency
        && telemetry_.generationCap < maxGeneratedFrames_;
    if (!provisionalLowerActive_
            && telemetry_.generationCap < maxGeneratedFrames_
            && (upwardProbePending_
                || genericRecoveryProbe
                || (deficitProbeEligible
                    && recoveryEvidence_ >= kWsiDeficitProbeThreshold))) {
        upwardProbeInFlight_ = true;
        upwardProbePending_ = false;
        upwardProbeAttempted_ = std::min(
            requested, telemetry_.generationCap + 1);
        if (upwardProbeAttempted_ > telemetry_.generationCap)
            return upwardProbeAttempted_;
        upwardProbeInFlight_ = false;
    }

    const std::size_t capped = std::min(requested, telemetry_.generationCap);
    if (capped != 1 || telemetry_.singleFrameDuty >= 0.999)
        return capped;

    // Once downstream capacity is below one synthetic frame per real source
    // cycle, turn the integer cap into a deterministic fractional duty cycle.
    // Suppressed slots are consumed here and never become scheduler debt.
    singleFramePhase_ += telemetry_.singleFrameDuty;
    if (singleFramePhase_ + 1e-9 < 1.0)
        return 0;

    singleFramePhase_ = std::max(0.0, singleFramePhase_ - 1.0);
    return 1;
}

void GeneratedPresentationCapacityTracker::observe(
        std::size_t attempted, std::size_t wsiRejected) {
    const std::size_t rejected = std::min(wsiRejected, attempted);
    const std::size_t accepted = attempted - rejected;
    observe(
        attempted,
        accepted,
        rejected,
        GeneratedPresentationCapacityContext{});
}

void GeneratedPresentationCapacityTracker::observe(
        std::size_t attempted,
        std::size_t accepted,
        std::size_t wsiRejected,
        const GeneratedPresentationCapacityContext& context) {
    telemetry_.lowered = false;
    telemetry_.raised = false;

    if (attempted == 0 || maxGeneratedFrames_ == 0)
        return;

    const std::size_t rejected = std::min(wsiRejected, attempted);
    accepted = std::min(accepted, attempted - rejected);
    const double efficiency =
        static_cast<double>(accepted) / static_cast<double>(attempted);
    const double sample =
        static_cast<double>(rejected) / static_cast<double>(attempted);
    telemetry_.wsiRejectionRatio = hasObservation_
        ? telemetry_.wsiRejectionRatio
            + 0.25 * (sample - telemetry_.wsiRejectionRatio)
        : sample;
    if (!hasObservation_) {
        acceptedFramesEwma_ = static_cast<double>(accepted);
        efficiencyEwma_ = efficiency;
    } else {
        acceptedFramesEwma_ += kWsiAcceptanceAlpha
            * (static_cast<double>(accepted) - acceptedFramesEwma_);
        efficiencyEwma_ += kWsiAcceptanceAlpha
            * (efficiency - efficiencyEwma_);
    }
    hasObservation_ = true;

    telemetry_.attemptedGeneratedFrames += attempted;
    telemetry_.acceptedGeneratedFrames += accepted;
    telemetry_.deliveredEfficiency = efficiencyEwma_;
    telemetry_.acceptedFramesEwma = acceptedFramesEwma_;
    if (accepted > 0 && attempted >= telemetry_.generationCap)
        telemetry_.highestUsefulCapacity = std::max(
            telemetry_.highestUsefulCapacity, attempted);

    if (rejected > 0) {
        rejectionEvidence_ = std::min(
            rejectionEvidence_ + kWsiEvidenceIncrement,
            kWsiEvidenceThreshold * 3);
        recoveryEvidence_ = std::max(
            0.0, recoveryEvidence_ - kWsiRecoveryEvidenceRejectDecay);
    } else {
        rejectionEvidence_ = rejectionEvidence_ > kWsiEvidenceDecay
            ? rejectionEvidence_ - kWsiEvidenceDecay
            : 0;
        if (efficiency >= kWsiRecoveryGoodEfficiency)
            recoveryEvidence_ += kWsiRecoveryEvidenceIncrement;
    }

    const bool provenHigherCapacity = higherCapacityIsProven(
        context, telemetry_.generationCap);
    if (context.outputDeficit && provenHigherCapacity)
        recoveryEvidence_ += 0.75;

    if (upwardProbeInFlight_) {
        if (accepted > telemetry_.generationCap) {
            ++telemetry_.generationCap;
            telemetry_.generationCap = std::min(
                telemetry_.generationCap, maxGeneratedFrames_);
            telemetry_.singleFrameDuty = 1.0;
            telemetry_.raised = true;
            telemetry_.lastChangeReason = context.outputDeficit
                ? GeneratedPresentationCapChangeReason::TargetDeficitProbeSuccess
                : GeneratedPresentationCapChangeReason::RecoveryEvidenceRaise;
            telemetry_.lastChangeOutputDeficit = context.outputDeficit;
            recoveryEvidence_ = 0.0;
            rejectionEvidence_ = 0;
            singleFramePhase_ = 0.0;
        } else {
            recoveryEvidence_ = std::max(0.0, recoveryEvidence_ - 1.0);
        }
        upwardProbeInFlight_ = false;
        upwardProbeAttempted_ = 0;
    }

    if (provisionalLowerActive_) {
        ++provisionalSamples_;
        provisionalAcceptedSum_ += static_cast<double>(accepted);
        provisionalEfficiencySum_ += efficiency;
        if (provisionalSamples_ >= kWsiProvisionalEvaluationSamples) {
            const double lowerAccepted = provisionalAcceptedSum_
                / static_cast<double>(provisionalSamples_);
            const double lowerEfficiency = provisionalEfficiencySum_
                / static_cast<double>(provisionalSamples_);
            const bool efficiencyImproved = lowerEfficiency
                >= provisionalBaselineEfficiency_ + kWsiEfficiencyImprovement;
            const bool throughputPreserved =
                provisionalBaselineAccepted_ <= 0.0
                || lowerAccepted >= provisionalBaselineAccepted_
                    * kWsiThroughputPreserveRatio;
            const bool lowerCapUnprofitable = context.outputDeficit
                ? (!throughputPreserved || !efficiencyImproved)
                : (!throughputPreserved && !efficiencyImproved);
            if (lowerCapUnprofitable) {
                telemetry_.generationCap = std::min(
                    provisionalPreviousCap_, maxGeneratedFrames_);
                telemetry_.raised = true;
                telemetry_.lastChangeReason =
                    GeneratedPresentationCapChangeReason::ProfitabilityRestoreHigher;
                telemetry_.lastChangeOutputDeficit = context.outputDeficit;
                recoveryEvidence_ = 0.0;
                rejectionEvidence_ = 0;
            } else {
                telemetry_.lastChangeReason =
                    GeneratedPresentationCapChangeReason::ProfitabilityKeepLower;
                telemetry_.lastChangeOutputDeficit = context.outputDeficit;
            }
            provisionalLowerActive_ = false;
            provisionalSamples_ = 0;
            provisionalAcceptedSum_ = 0.0;
            provisionalEfficiencySum_ = 0.0;
        }
    }

    const bool canLowerIntegerCap =
        !provisionalLowerActive_
        && telemetry_.generationCap > 1
        && rejectionEvidence_ >= kWsiEvidenceThreshold
        && telemetry_.wsiRejectionRatio >= kWsiPressureRatio;
    if (canLowerIntegerCap) {
        provisionalLowerActive_ = true;
        provisionalPreviousCap_ = telemetry_.generationCap;
        provisionalBaselineAccepted_ = acceptedFramesEwma_;
        provisionalBaselineEfficiency_ = efficiencyEwma_;
        provisionalSamples_ = 0;
        provisionalAcceptedSum_ = 0.0;
        provisionalEfficiencySum_ = 0.0;
        --telemetry_.generationCap;
        telemetry_.lowered = true;
        telemetry_.lastChangeReason =
            GeneratedPresentationCapChangeReason::RejectionProbe;
        telemetry_.lastChangeOutputDeficit = context.outputDeficit;
        rejectionEvidence_ = 1;
        singleFramePhase_ = 0.0;
    } else if (!provisionalLowerActive_
            && telemetry_.generationCap == 1
            && rejectionEvidence_ >= kWsiEvidenceThreshold
            && telemetry_.wsiRejectionRatio
                >= kWsiSevereSingleFramePressureRatio
            && !(context.outputDeficit
                && context.sourceInsideBudget
                && provenHigherCapacity)
            && telemetry_.singleFrameDuty
                > kSingleFrameDuties.back() + 1e-6) {
        telemetry_.singleFrameDuty =
            nextLowerDuty(telemetry_.singleFrameDuty);
        telemetry_.lowered = true;
        telemetry_.lastChangeReason =
            GeneratedPresentationCapChangeReason::SubOneDutyLower;
        telemetry_.lastChangeOutputDeficit = context.outputDeficit;
        rejectionEvidence_ = 0;
        recoveryEvidence_ = 0.0;
        singleFramePhase_ = 0.0;
    }

    if (telemetry_.singleFrameDuty < 0.999
            && recoveryEvidence_ >= kWsiRecoveryProbeThreshold) {
            telemetry_.singleFrameDuty =
                nextHigherDuty(telemetry_.singleFrameDuty);
            telemetry_.raised = true;
            telemetry_.lastChangeReason =
                GeneratedPresentationCapChangeReason::SubOneDutyRecover;
            telemetry_.lastChangeOutputDeficit = context.outputDeficit;
            recoveryEvidence_ = 0.0;
            singleFramePhase_ = 0.0;
    } else if (!provisionalLowerActive_
            && telemetry_.generationCap < maxGeneratedFrames_
            && recoveryEvidence_ >= kWsiRecoveryProbeThreshold) {
        if (!context.outputDeficit && !context.deadlineCapacityValid) {
            ++telemetry_.generationCap;
            telemetry_.raised = true;
            telemetry_.lastChangeReason =
                GeneratedPresentationCapChangeReason::RecoveryEvidenceRaise;
            telemetry_.lastChangeOutputDeficit = false;
            recoveryEvidence_ = 0.0;
        } else {
            upwardProbePending_ = true;
        }
    } else if (context.outputDeficit
            && provenHigherCapacity
            && recoveryEvidence_ >= kWsiDeficitProbeThreshold) {
        upwardProbePending_ = true;
    }

    telemetry_.rejectionEvidence = rejectionEvidence_;
    telemetry_.recoveryEvidence = recoveryEvidence_;
    telemetry_.highestUsefulCapacity = std::max(
        telemetry_.highestUsefulCapacity,
        accepted > 0 ? attempted : 0U);
    telemetry_.upwardProbePending = upwardProbePending_;
    telemetry_.provisionalLowerActive = provisionalLowerActive_;
    telemetry_.pressure =
        rejected > 0
        || telemetry_.wsiRejectionRatio >= 0.10
        || provisionalLowerActive_
        || telemetry_.generationCap < maxGeneratedFrames_
        || telemetry_.singleFrameDuty < 0.999;
}

void GeneratedPresentationCapacityTracker::reset() {
    const auto configuredMax = maxGeneratedFrames_;
    maxGeneratedFrames_ = std::numeric_limits<std::size_t>::max();
    configure(configuredMax);
}

void LsfgOutputCadenceTracker::configure(bool targeted, uint32_t targetFps) {
    if (targeted_ == targeted && targetFps_ == targetFps)
        return;
    reset();
    targeted_ = targeted;
    targetFps_ = targetFps;
    snapshot_.targeted = targeted_;
    if (!targeted_)
        snapshot_.targetSatisfiedConfirmed = true;
}

void LsfgOutputCadenceTracker::clearWindow() {
    sampleCount_ = 0;
    nextSample_ = 0;
    deficitSeconds_ = 0.0;
    satisfiedSeconds_ = 0.0;
    snapshot_ = {};
    snapshot_.targeted = targeted_;
    if (!targeted_)
        snapshot_.targetSatisfiedConfirmed = true;
}

void LsfgOutputCadenceTracker::rebuildSnapshot(double evidenceSeconds) {
    constexpr double kWindowSeconds = 0.40;
    constexpr double kMinimumCoverageSeconds = 0.25;
    constexpr double kDeficitConfirmSeconds = 0.30;
    constexpr double kSatisfiedConfirmSeconds = 0.50;
    constexpr double kDeficitRatio = 0.97;
    constexpr double kSatisfiedRatio = 0.985;

    double seconds = 0.0;
    std::size_t frames = 0;
    for (std::size_t offset = 0;
            offset < sampleCount_ && seconds < kWindowSeconds;
            ++offset) {
        const std::size_t index =
            (nextSample_ + kSampleCapacity - 1 - offset) % kSampleCapacity;
        seconds += samples_[index].seconds;
        frames += samples_[index].frames;
    }

    snapshot_.targeted = targeted_;
    snapshot_.coverageSeconds = seconds;
    snapshot_.valid =
        seconds >= kMinimumCoverageSeconds && frames > 0;
    snapshot_.outputFps = snapshot_.valid
        ? static_cast<double>(frames) / seconds
        : 0.0;

    if (!targeted_) {
        snapshot_.deficitConfirmed = false;
        snapshot_.targetSatisfiedConfirmed = true;
        return;
    }
    if (!snapshot_.valid || targetFps_ == 0) {
        snapshot_.deficitConfirmed = false;
        snapshot_.targetSatisfiedConfirmed = false;
        return;
    }

    const double target = static_cast<double>(targetFps_);
    if (snapshot_.outputFps < target * kDeficitRatio) {
        deficitSeconds_ += evidenceSeconds;
        satisfiedSeconds_ = 0.0;
    } else if (snapshot_.outputFps >= target * kSatisfiedRatio) {
        satisfiedSeconds_ += evidenceSeconds;
        deficitSeconds_ = 0.0;
    } else {
        deficitSeconds_ = 0.0;
        satisfiedSeconds_ = 0.0;
    }

    snapshot_.deficitConfirmed =
        deficitSeconds_ >= kDeficitConfirmSeconds;
    snapshot_.targetSatisfiedConfirmed =
        satisfiedSeconds_ >= kSatisfiedConfirmSeconds;
}

void LsfgOutputCadenceTracker::observe(
        std::chrono::nanoseconds elapsed,
        std::size_t sourceFrames,
        std::size_t generatedFrames) {
    const double seconds = std::chrono::duration<double>(elapsed).count();
    if (!(seconds > 0.0) || !std::isfinite(seconds))
        return;

    // Treat a suspend/menu pause as stale evidence, not as a giant low-output
    // sample. The runtime's cadence-relative discontinuity logic remains the
    // source of truth for scheduler resets.
    if (seconds >= 0.250) {
        clearWindow();
        return;
    }

    samples_[nextSample_] = Sample{
        .seconds = seconds,
        .frames = sourceFrames + generatedFrames,
    };
    nextSample_ = (nextSample_ + 1) % kSampleCapacity;
    sampleCount_ = std::min(sampleCount_ + 1, kSampleCapacity);
    rebuildSnapshot(std::min(seconds, 0.050));
}

void LsfgOutputCadenceTracker::reset() {
    targeted_ = false;
    targetFps_ = 0;
    clearWindow();
}

std::size_t FixedSourceCadenceGovernor::plan(
        std::chrono::nanoseconds sourceInterval,
        std::size_t requestedGeneratedFrames,
        std::size_t previousDispatchedGeneratedFrames,
        bool generationAllowed,
        SourceCadenceObservation previousObservation) {
    constexpr double kPressureIntervalRatio = 1.25;
    constexpr double kStableIntervalRatio = 1.10;
    constexpr double kPressureConfirmSeconds = 0.12;
    constexpr double kRecoveryConfirmSeconds = 0.30;
    constexpr double kBackoffCooldownSeconds = 0.50;
    constexpr double kSourceBaselineAlpha = 0.35;
    constexpr double kMaxEvidenceSeconds = 0.25;

    telemetry_.requestedGeneratedFrames = requestedGeneratedFrames;
    telemetry_.backedOff = false;
    telemetry_.raised = false;

    const std::size_t previousRequested = requestedGeneratedFrames_;
    const bool requestRaised =
        requestedGeneratedFrames > previousRequested;
    requestedGeneratedFrames_ = requestedGeneratedFrames;

    if (requestedGeneratedFrames == 0) {
        generationLimit_ = 0;
        backedOffActive_ = false;
        pressureSeconds_ = 0.0;
        recoverySeconds_ = 0.0;
        cooldownSeconds_ = 0.0;
        telemetry_.generationLimit = 0;
        telemetry_.baselineValid = hasBaseline_;
        telemetry_.baselineSourceFps =
            hasBaseline_ && baselineIntervalSeconds_ > 0.0
                ? 1.0 / baselineIntervalSeconds_
                : 0.0;
        return 0;
    }

    // A newly requested higher Fixed multiplier is explicit user intent.
    // Start at that requested cost immediately; only subsequent, confirmed
    // source-cadence degradation may back it off.
    if (requestRaised) {
        generationLimit_ = requestedGeneratedFrames;
        backedOffActive_ = false;
        pressureSeconds_ = 0.0;
        recoverySeconds_ = 0.0;
        cooldownSeconds_ = 0.0;
    } else if (generationLimit_ > requestedGeneratedFrames) {
        generationLimit_ = requestedGeneratedFrames;
        if (generationLimit_ == requestedGeneratedFrames)
            backedOffActive_ = false;
    }

    const double intervalSeconds =
        std::chrono::duration<double>(sourceInterval).count();
    const bool intervalValid =
        intervalSeconds > 0.0 && std::isfinite(intervalSeconds);

    if (intervalValid) {
        const double evidenceSeconds =
            std::min(intervalSeconds, kMaxEvidenceSeconds);
        cooldownSeconds_ = std::max(0.0, cooldownSeconds_ - evidenceSeconds);

        if (!hasBaseline_) {
            // The first trustworthy source interval is measurement only.
            // The configured Fixed multiplier has already seeded
            // generationLimit_, but generation remains blocked until a clean
            // baseline exists and the caller allows interpolation.
            baselineIntervalSeconds_ = intervalSeconds;
            hasBaseline_ = true;
            pressureSeconds_ = 0.0;
            recoverySeconds_ = 0.0;
            telemetry_.intervalRatio = 1.0;
            telemetry_.baselineValid = true;
            telemetry_.baselineSourceFps = 1.0 / baselineIntervalSeconds_;
            telemetry_.generationLimit = generationLimit_;
            return 0;
        }

        double intervalRatio =
            intervalSeconds / baselineIntervalSeconds_;
        telemetry_.intervalRatio = intervalRatio;

        if (previousDispatchedGeneratedFrames == 0) {
            pressureSeconds_ = 0.0;

            // Only a genuine source-only observation is allowed to move the
            // baseline. History maintenance is still LSFG work and generated
            // cycles must never normalize their own cost into the baseline.
            if (previousObservation == SourceCadenceObservation::SourceOnly) {
                baselineIntervalSeconds_ += kSourceBaselineAlpha
                    * (intervalSeconds - baselineIntervalSeconds_);
                intervalRatio = intervalSeconds / baselineIntervalSeconds_;
                telemetry_.intervalRatio = intervalRatio;
            }

            recoverySeconds_ =
                intervalRatio <= kStableIntervalRatio
                    && cooldownSeconds_ <= 0.0
                ? recoverySeconds_ + evidenceSeconds
                : 0.0;
        } else if (intervalRatio >= kPressureIntervalRatio) {
            pressureSeconds_ += evidenceSeconds;
            recoverySeconds_ = 0.0;
            if (pressureSeconds_ >= kPressureConfirmSeconds) {
                const std::size_t saferLimit =
                    previousDispatchedGeneratedFrames > 0
                        ? previousDispatchedGeneratedFrames - 1
                        : 0;
                if (saferLimit < generationLimit_) {
                    generationLimit_ = saferLimit;
                    backedOffActive_ = true;
                    telemetry_.backedOff = true;
                }
                cooldownSeconds_ = kBackoffCooldownSeconds;
                pressureSeconds_ = 0.0;
            }
        } else {
            pressureSeconds_ = 0.0;
            recoverySeconds_ =
                intervalRatio <= kStableIntervalRatio
                    && cooldownSeconds_ <= 0.0
                ? recoverySeconds_ + evidenceSeconds
                : 0.0;
        }

        if (generationAllowed
                && backedOffActive_
                && generationLimit_ < requestedGeneratedFrames
                && cooldownSeconds_ <= 0.0
                && recoverySeconds_ >= kRecoveryConfirmSeconds) {
            ++generationLimit_;
            recoverySeconds_ = 0.0;
            telemetry_.raised = true;
            if (generationLimit_ >= requestedGeneratedFrames)
                backedOffActive_ = false;
        }
    }

    telemetry_.baselineValid = hasBaseline_;
    telemetry_.baselineSourceFps =
        hasBaseline_ && baselineIntervalSeconds_ > 0.0
            ? 1.0 / baselineIntervalSeconds_
            : 0.0;
    telemetry_.generationLimit = generationLimit_;

    if (!generationAllowed || !hasBaseline_)
        return 0;
    return std::min(generationLimit_, requestedGeneratedFrames);
}

void FixedSourceCadenceGovernor::reset() {
    hasBaseline_ = false;
    baselineIntervalSeconds_ = 0.0;
    generationLimit_ = 0;
    requestedGeneratedFrames_ = 0;
    backedOffActive_ = false;
    pressureSeconds_ = 0.0;
    recoverySeconds_ = 0.0;
    cooldownSeconds_ = 0.0;
    telemetry_ = {};
    telemetry_.generationLimit = generationLimit_;
}


AdaptiveFrameScheduler::AdaptiveFrameScheduler(
        uint32_t targetFps, std::size_t maxGeneratedFrames)
        : targetFps_(targetFps),
          maxGeneratedFrames_(maxGeneratedFrames) {
    resetRuntimeState();
}

void AdaptiveFrameScheduler::configure(
        uint32_t targetFps, std::size_t maxGeneratedFrames) {
    if (targetFps_ == targetFps && maxGeneratedFrames_ == maxGeneratedFrames)
        return;

    // A suspend-spanning interval may already have cleared the current EMA
    // before the render thread gets a chance to observe the Quick Menu's atomic
    // conf.toml update. Keep a separate lifecycle-level cadence bit so the
    // user's target change is still treated as an established-runtime
    // reconfiguration rather than a first-start cold configuration.
    const bool hadRuntimeCadence = runtimeCadenceEstablished_;
    const bool wasActive = targetFps_ != 0 && maxGeneratedFrames_ != 0;
    targetFps_ = targetFps;
    maxGeneratedFrames_ = maxGeneratedFrames;
    resetRuntimeState();

    // A Quick Menu target/multiplier change is an explicit user request, not a
    // scene-rate inference. Preserve that intent across the menu's
    // suspend/resume discontinuity so the first valid cadence sample can seed
    // the requested generation ceiling immediately.
    reconfigureWarmStartPending_ = hadRuntimeCadence
        && wasActive
        && targetFps_ != 0
        && maxGeneratedFrames_ != 0;
}

void AdaptiveFrameScheduler::setSafeGenerationHint(
        std::size_t hint, bool valid) {
    safeGenerationHintValid_ = valid;
    safeGenerationHint_ = valid
        ? std::min(hint, maxGeneratedFrames_)
        : 0;
}

void AdaptiveFrameScheduler::setGenerationFirst(bool enabled) {
    generationFirst_ = enabled;
    if (!generationFirst_)
        return;

    costLimit_ = maxGeneratedFrames_;
    resetUnmetDemand();
}

double AdaptiveFrameScheduler::stabilizeGenerationDensity(
        double desiredDensity) {
    desiredDensity = std::clamp(
        desiredDensity,
        0.0,
        static_cast<double>(maxGeneratedFrames_));

    if (integerDensityLocked_) {
        const double lockedDensity =
            static_cast<double>(lockedIntegerDensity_);
        if (std::abs(desiredDensity - lockedDensity)
                <= kIntegerDensityReleaseWindow) {
            integerDensityReleaseSamples_ = 0;
            integerDensityCandidate_ = 0;
            integerDensityCandidateSamples_ = 0;
            return lockedDensity;
        }

        ++integerDensityReleaseSamples_;
        if (integerDensityReleaseSamples_ < kIntegerDensityReleaseSamples)
            return lockedDensity;

        integerDensityLocked_ = false;
        lockedIntegerDensity_ = 0;
        integerDensityReleaseSamples_ = 0;
        integerDensityCandidate_ = 0;
        integerDensityCandidateSamples_ = 0;
        // Never carry integer-regime phase into a genuinely fractional regime.
        fractionalOpportunityPhase_ = 0.0;
    }

    const double nearestInteger = std::round(desiredDensity);
    const bool integerCandidateValid =
        nearestInteger >= 1.0
        && nearestInteger <= static_cast<double>(maxGeneratedFrames_)
        && std::abs(desiredDensity - nearestInteger)
            <= kIntegerDensityAcquireWindow;

    if (!integerCandidateValid) {
        integerDensityCandidate_ = 0;
        integerDensityCandidateSamples_ = 0;
        return desiredDensity;
    }

    const auto candidate =
        static_cast<std::size_t>(nearestInteger);
    if (integerDensityCandidate_ == candidate) {
        ++integerDensityCandidateSamples_;
    } else {
        integerDensityCandidate_ = candidate;
        integerDensityCandidateSamples_ = 1;
    }

    if (integerDensityCandidateSamples_ >= kIntegerDensityAcquireSamples) {
        integerDensityLocked_ = true;
        lockedIntegerDensity_ = candidate;
        integerDensityCandidate_ = 0;
        integerDensityCandidateSamples_ = 0;
        integerDensityReleaseSamples_ = 0;
        fractionalOpportunityPhase_ = 0.0;
        return static_cast<double>(lockedIntegerDensity_);
    }

    return desiredDensity;
}

std::size_t AdaptiveFrameScheduler::plan(std::chrono::nanoseconds sourceInterval) {
    telemetry_.sourceRateSnapped = false;
    telemetry_.costRaised = false;
    telemetry_.costBackedOff = false;
    telemetry_.costProbe = false;
    telemetry_.discontinuityReset = false;
    telemetry_.configWarmStart = false;
    telemetry_.capacityPromoted = false;
    telemetry_.safeGenerationHintValid = safeGenerationHintValid_;
    telemetry_.safeGenerationHint = safeGenerationHint_;
    telemetry_.generatedFrames = 0;
    telemetry_.wantedGeneratedFrames = 0.0;
    telemetry_.scheduledGenerationDensity = 0.0;
    telemetry_.fractionalPhase = fractionalOpportunityPhase_;
    telemetry_.integerDensityLocked = integerDensityLocked_;
    telemetry_.lockedGeneratedFrames = lockedIntegerDensity_;
    telemetry_.densityTransitionEvidence = integerDensityLocked_
        ? integerDensityReleaseSamples_
        : integerDensityCandidateSamples_;
    telemetry_.syntheticOpportunitiesCreated = 0;

    if (targetFps_ == 0 || maxGeneratedFrames_ == 0) {
        telemetry_.costLimit = 0;
        return 0;
    }

    const double intervalSeconds = std::chrono::duration<double>(sourceInterval).count();
    if (!(intervalSeconds > 0.0) || !std::isfinite(intervalSeconds))
        return 0;

    // Distinguish a suspend/stall from a legitimately slow source by comparing
    // against established cadence rather than an absolute FPS band. The first
    // sample at any source rate is always eligible. After an extreme outlier,
    // consume that one sample as a discontinuity and let the next real interval
    // establish the new cadence without synthetic catch-up debt.
    if (lastTrustedSourceIntervalSeconds_ > 0.0
            && intervalSeconds
                > lastTrustedSourceIntervalSeconds_ * kDiscontinuityRatio) {
        const bool preserveWarmStart = reconfigureWarmStartPending_;
        resetRuntimeState();
        lastTrustedSourceIntervalSeconds_ = 0.0;
        reconfigureWarmStartPending_ = preserveWarmStart;
        telemetry_.discontinuityReset = true;
        return 0;
    }

    // This bit intentionally survives later timing discontinuities. It is only
    // cleared by an explicit lifecycle reset(), so config writes observed just
    // after resume can still distinguish a running game from first startup.
    runtimeCadenceEstablished_ = true;
    observedTimeSeconds_ += intervalSeconds;
    updateSourceRate(intervalSeconds);
    lastTrustedSourceIntervalSeconds_ = smoothedSourceIntervalSeconds_;

    const double wantedGenerated = std::clamp(
        static_cast<double>(targetFps_) * smoothedSourceIntervalSeconds_ - 1.0,
        0.0,
        static_cast<double>(maxGeneratedFrames_));
    telemetry_.wantedGeneratedFrames = wantedGenerated;

    if (reconfigureWarmStartPending_) {
        // The first trustworthy post-resume interval is the earliest point at
        // which the new target can be translated into an actual interpolation
        // cost. Seed directly to that bounded requirement: a later source-rate
        // drop increases target demand rather than undoing the user's target.
        const auto requiredCost = static_cast<std::size_t>(std::clamp(
            std::ceil(wantedGenerated - 1e-6),
            1.0,
            static_cast<double>(maxGeneratedFrames_)));
        costLimit_ = std::max(costLimit_, requiredCost);
        resetUnmetDemand();
        reconfigureWarmStartPending_ = false;
        telemetry_.configWarmStart = true;
    }

    updateCostLimit(wantedGenerated);
    telemetry_.costLimit = costLimit_;

    // Generation density follows the protected/smoothed source cadence.
    // Raw sourceInterval remains useful for cadence estimation, discontinuity
    // detection, and pressure evidence, but it must not directly change the
    // number of synthetics produced by one cycle.
    const double scheduledDensity =
        stabilizeGenerationDensity(wantedGenerated);
    telemetry_.scheduledGenerationDensity = scheduledDensity;
    telemetry_.opportunityIntervalSeconds =
        smoothedSourceIntervalSeconds_;
    telemetry_.integerDensityLocked = integerDensityLocked_;
    telemetry_.lockedGeneratedFrames = lockedIntegerDensity_;
    telemetry_.densityTransitionEvidence = integerDensityLocked_
        ? integerDensityReleaseSamples_
        : integerDensityCandidateSamples_;

    std::size_t wholeOpportunities = 0;
    if (integerDensityLocked_) {
        // An integer target/source regime is intentionally phase-free. Capacity
        // may still lower this later, but ordinary source jitter cannot turn
        // 3 generated/source into a 3/2/3 cadence pattern.
        fractionalOpportunityPhase_ = 0.0;
        wholeOpportunities = lockedIntegerDensity_;
    } else {
        // Genuine fractional ratios use deterministic error diffusion from the
        // smoothed desired density. Downstream/cost rejection consumes the
        // opportunity now; it never becomes catch-up debt.
        fractionalOpportunityPhase_ = std::max(
            0.0,
            fractionalOpportunityPhase_ + scheduledDensity);

        constexpr double kIntegerSnapEpsilon = 1e-6;
        wholeOpportunities = static_cast<std::size_t>(std::floor(
            fractionalOpportunityPhase_ + kIntegerSnapEpsilon));
        fractionalOpportunityPhase_ -=
            static_cast<double>(wholeOpportunities);
        fractionalOpportunityPhase_ = std::clamp(
            fractionalOpportunityPhase_, 0.0, 0.999999);
    }

    // Desired cadence and sustainable capacity are separate decisions.
    const std::size_t opportunities = std::min(
        { wholeOpportunities, costLimit_, maxGeneratedFrames_ });

    telemetry_.fractionalPhase = fractionalOpportunityPhase_;
    telemetry_.syntheticOpportunitiesCreated = opportunities;
    telemetry_.generatedFrames = opportunities;
    return opportunities;
}

void AdaptiveFrameScheduler::resetSourceCadenceWindow() {
    recentSourceIntervals_.fill(0.0);
    recentSourceIntervalCount_ = 0;
    recentSourceIntervalCursor_ = 0;
}

void AdaptiveFrameScheduler::resetUnmetDemand() {
    unmetDemandSinceSeconds_ = -1.0;
    capacityRaiseSamples_ = 0;
}

double AdaptiveFrameScheduler::robustSourceIntervalSeconds() const {
    if (recentSourceIntervalCount_ == 0)
        return 0.0;

    std::array<double, kSourceCadenceWindow> sorted{};
    for (std::size_t i = 0; i < recentSourceIntervalCount_; ++i)
        sorted[i] = recentSourceIntervals_[i];
    std::sort(
        sorted.begin(),
        sorted.begin() + static_cast<std::ptrdiff_t>(recentSourceIntervalCount_));

    std::size_t begin = 0;
    std::size_t end = recentSourceIntervalCount_;
    if (recentSourceIntervalCount_ >= 7) {
        // One high and one low outlier are discarded. This is enough to absorb
        // isolated Android/WSI bursts and 80-100 ms source hitches without
        // hiding a sustained cadence transition.
        begin = 1;
        end -= 1;
    }

    double sum = 0.0;
    for (std::size_t i = begin; i < end; ++i)
        sum += sorted[i];
    return sum / static_cast<double>(end - begin);
}

void AdaptiveFrameScheduler::updateSourceRate(double intervalSeconds) {
    telemetry_.sourceFps = 1.0 / intervalSeconds;

    recentSourceIntervals_[recentSourceIntervalCursor_] = intervalSeconds;
    recentSourceIntervalCursor_ =
        (recentSourceIntervalCursor_ + 1) % kSourceCadenceWindow;
    recentSourceIntervalCount_ =
        std::min(recentSourceIntervalCount_ + 1, kSourceCadenceWindow);

    const double robustInterval = robustSourceIntervalSeconds();
    bool cadenceStableThisSample = false;
    if (recentSourceIntervalCount_ >= kCapacityCadenceWindow
            && robustInterval > 0.0) {
        double recentMin = std::numeric_limits<double>::max();
        double recentMax = 0.0;
        for (std::size_t offset = 0;
                offset < kCapacityCadenceWindow;
                ++offset) {
            const std::size_t index =
                (recentSourceIntervalCursor_ + kSourceCadenceWindow - 1 - offset)
                % kSourceCadenceWindow;
            recentMin = std::min(recentMin, recentSourceIntervals_[index]);
            recentMax = std::max(recentMax, recentSourceIntervals_[index]);
        }
        cadenceStableThisSample =
            recentMax - recentMin
                <= robustInterval * kCapacityCadenceDeviationRatio;
    }
    if (cadenceStableThisSample)
        ++stableCadenceSamples_;
    else
        stableCadenceSamples_ = 0;

    if (!hasSmoothedInterval_) {
        smoothedSourceIntervalSeconds_ = robustInterval;
        hasSmoothedInterval_ = robustInterval > 0.0;
    } else if (robustInterval > 0.0) {
        const double previous = smoothedSourceIntervalSeconds_;
        const double boundedTarget = std::clamp(
            robustInterval,
            previous * kCadenceTargetMinRatio,
            previous * kCadenceTargetMaxRatio);
        const double alpha = boundedTarget > previous
            ? kSlowdownCadenceAlpha
            : kSpeedupCadenceAlpha;
        smoothedSourceIntervalSeconds_ +=
            alpha * (boundedTarget - smoothedSourceIntervalSeconds_);
    }

    telemetry_.smoothedSourceFps = smoothedSourceIntervalSeconds_ > 0.0
        ? 1.0 / smoothedSourceIntervalSeconds_
        : 0.0;
}

void AdaptiveFrameScheduler::updateCostLimit(
        double wantedGeneratedFrames) {
    if (maxGeneratedFrames_ == 0) {
        costLimit_ = 0;
        resetUnmetDemand();
        return;
    }

    if (generationFirst_) {
        costLimit_ = maxGeneratedFrames_;
        resetUnmetDemand();
        return;
    }

    if (costLimit_ == 0)
        costLimit_ = 1;
    costLimit_ = std::min(costLimit_, maxGeneratedFrames_);

    if (costLimit_ >= maxGeneratedFrames_) {
        resetUnmetDemand();
        return;
    }

    if (wantedGeneratedFrames <= static_cast<double>(costLimit_) + 0.001) {
        resetUnmetDemand();
        return;
    }

    if (unmetDemandSinceSeconds_ < 0.0)
        unmetDemandSinceSeconds_ = observedTimeSeconds_;

    const bool capacitySupportsNext =
        safeGenerationHintValid_
        && safeGenerationHint_ >= costLimit_ + 1;
    if (capacitySupportsNext)
        ++capacityRaiseSamples_;
    else
        capacityRaiseSamples_ = 0;

    const bool capacityPromotionReady =
        capacityRaiseSamples_ >= kCapacityRaiseSamplesRequired
        && stableCadenceSamples_ > 0;

    if (!capacityPromotionReady
            && observedTimeSeconds_ - unmetDemandSinceSeconds_
                < kSustainedDemandSeconds) {
        return;
    }

    costLimit_++;
    telemetry_.capacityPromoted = capacityPromotionReady;
    resetUnmetDemand();
    telemetry_.costRaised = true;
}

void AdaptiveFrameScheduler::resetRuntimeState() {
    fractionalOpportunityPhase_ = 0.0;
    integerDensityLocked_ = false;
    lockedIntegerDensity_ = 0;
    integerDensityCandidate_ = 0;
    integerDensityCandidateSamples_ = 0;
    integerDensityReleaseSamples_ = 0;
    smoothedSourceIntervalSeconds_ = 0.0;
    hasSmoothedInterval_ = false;
    reconfigureWarmStartPending_ = false;
    resetSourceCadenceWindow();
    safeGenerationHint_ = 0;
    safeGenerationHintValid_ = false;
    stableCadenceSamples_ = 0;
    observedTimeSeconds_ = 0.0;
    costLimit_ = maxGeneratedFrames_ == 0
        ? 0
        : (generationFirst_ ? maxGeneratedFrames_ : 1);
    resetUnmetDemand();
    telemetry_ = {};
    telemetry_.costLimit = costLimit_;
}

void AdaptiveFrameScheduler::reset() {
    runtimeCadenceEstablished_ = false;
    lastTrustedSourceIntervalSeconds_ = 0.0;
    resetRuntimeState();
}
