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
constexpr unsigned kRaiseBackoffSamplesRequired = 3;
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
    if (discontinuity
            || (initialized_
                && lastIntervalNs_ > 0
                && intervalNs
                    > lastIntervalNs_ * kSourceTimelineDiscontinuityRatio)) {
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

void DeadlineAdmissionPredictor::observe(
        const DeadlineAdmissionObservation& observation) {
    if (!observation.valid
            || observation.generationCount == 0
            || !std::isfinite(observation.mipmapsMs)
            || !std::isfinite(observation.opticalFlowMs)
            || !std::isfinite(observation.totalLsfgMs)
            || observation.mipmapsMs < 0.0
            || observation.opticalFlowMs < 0.0
            || observation.totalLsfgMs <= 0.0) {
        return;
    }

    // AdaptiveFlowGpuTiming::opticalFlowMs is cumulative through the
    // optical-flow preprocess and already includes mipmaps. Treat it as the
    // shared pre-generation cost; adding mipmaps again would double-count the
    // same GPU interval and systematically overpredict synthetic work.
    const double sharedMs =
        std::max(observation.mipmapsMs, observation.opticalFlowMs);
    const double generatedResidualMs = std::max(
        0.0, observation.totalLsfgMs - sharedMs);
    const double perGeneratedMs = generatedResidualMs
        / static_cast<double>(observation.generationCount);

    const auto blend = [&](double current, double sample) {
        return hasEstimate_
            ? current + kEwmaAlpha * (sample - current)
            : sample;
    };

    mipmapsMs_ = blend(mipmapsMs_, observation.mipmapsMs);
    opticalFlowMs_ = blend(opticalFlowMs_, observation.opticalFlowMs);
    perGeneratedMs_ = blend(perGeneratedMs_, perGeneratedMs);
    hasEstimate_ = true;
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
            || !(usableBudgetMs > 0.0)
            || !std::isfinite(usableBudgetMs)) {
        return decision;
    }

    decision.predictedMipmapsMs = mipmapsMs_;
    decision.predictedOpticalFlowMs = opticalFlowMs_;
    decision.predictedTotalLsfgMs =
        std::max(mipmapsMs_, opticalFlowMs_)
        + perGeneratedMs_ * static_cast<double>(generationCount);
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

void DeadlineAdmissionPredictor::reset() {
    hasEstimate_ = false;
    mipmapsMs_ = 0.0;
    opticalFlowMs_ = 0.0;
    perGeneratedMs_ = 0.0;
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
    return context.deadlineCapacityValid
        && context.safeGenerationHint > currentCap
        && context.schedulerCostLimit > currentCap
        && context.sourceInsideBudget
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
    telemetry_.fractionalPhase = fractionalOpportunityPhase_;
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

    // Drive synthetic opportunities from elapsed source time rather than
    // repeatedly fractionalizing the smoothed source-rate estimate. Every real
    // source interval contributes the number of target-output frames that
    // elapsed during that interval, then consumes one slot for the real source
    // frame itself. The residual phase is the only state carried forward.
    //
    // This behaves like a time-domain error diffuser: long source intervals get
    // interpolation immediately, short intervals get less, and rejected/capped
    // whole opportunities are consumed now instead of becoming catch-up debt.
    // A single source hitch may consume elapsed wall time but it may not mint a
    // burst of synthetic target slots. Bound opportunity creation to the robust
    // predicted cadence and deliberately discard the excess elapsed time.
    const double opportunityIntervalSeconds = std::min(
        intervalSeconds,
        smoothedSourceIntervalSeconds_ * kOpportunityIntervalMaxRatio);
    telemetry_.opportunityIntervalSeconds = opportunityIntervalSeconds;
    const double intervalOutputDemand =
        static_cast<double>(targetFps_) * opportunityIntervalSeconds;
    fractionalOpportunityPhase_ = std::max(
        0.0,
        fractionalOpportunityPhase_ + intervalOutputDemand - 1.0);

    // Nanosecond source intervals such as 33,333,333 ns cannot represent
    // exact rational frame periods in binary floating point. Snap values that
    // are within one part per million of the next whole opportunity so stable
    // integer cadence ratios do not alternate 0/1 from representation error.
    constexpr double kIntegerSnapEpsilon = 1e-6;
    const auto wholeOpportunities = static_cast<std::size_t>(std::floor(
        fractionalOpportunityPhase_ + kIntegerSnapEpsilon));
    fractionalOpportunityPhase_ -= static_cast<double>(wholeOpportunities);
    fractionalOpportunityPhase_ = std::clamp(
        fractionalOpportunityPhase_, 0.0, 0.999999);

    // The long-term cost governor remains the sustainable work ceiling. Any
    // target-lattice opportunity above that ceiling is deliberately consumed,
    // not deferred, so a later cheap frame cannot repay old generation debt.
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

void AdaptiveFrameScheduler::updateCostLimit(double wantedGeneratedFrames) {
    if (maxGeneratedFrames_ == 0) {
        costLimit_ = 0;
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

    // Adaptive FG is target-seeking. Source cadence determines
    // wantedGeneratedFrames, so a slower source creates more interpolation
    // demand instead of triggering a protective generation backoff.
    //
    // The safe-generation hint may promote one level early when measured GPU
    // cost proves that level fits, but lack of a hint never vetoes the target.
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
    smoothedSourceIntervalSeconds_ = 0.0;
    hasSmoothedInterval_ = false;
    reconfigureWarmStartPending_ = false;
    resetSourceCadenceWindow();
    safeGenerationHint_ = 0;
    safeGenerationHintValid_ = false;
    stableCadenceSamples_ = 0;
    observedTimeSeconds_ = 0.0;
    costLimit_ = maxGeneratedFrames_ == 0 ? 0 : 1;
    resetUnmetDemand();
    telemetry_ = {};
    telemetry_.costLimit = costLimit_;
}

void AdaptiveFrameScheduler::reset() {
    runtimeCadenceEstablished_ = false;
    lastTrustedSourceIntervalSeconds_ = 0.0;
    resetRuntimeState();
}
