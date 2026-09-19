#include "adaptive_scheduler.hpp"

#include <algorithm>
#include <cmath>

namespace {
constexpr double kIntervalSmoothing = 0.15;
constexpr double kSlowIntervalHigh = 1.40;
constexpr double kFastIntervalLow = 0.70;
constexpr unsigned kSlowSamplesRequired = 3;
constexpr unsigned kFastSamplesRequired = 6;
// Treat a single interval as a suspend/stall discontinuity only when it is an
// extreme outlier relative to an already-established source cadence. An
// absolute FPS threshold would incorrectly disable generation for legitimately
// slow sources.
constexpr double kDiscontinuityRatio = 8.0;
constexpr uint64_t kSourceTimelineDiscontinuityRatio = 8ULL;

// Governor timing intentionally favors stability over quickly chasing an
// unreachable output target. The source-rate estimator is allowed to settle
// before additional GPU work is introduced.
constexpr double kSustainedDemandSeconds = 0.600;
constexpr double kPostRateChangeRaiseHoldSeconds = 0.750;
constexpr double kProbeIntervalSeconds = 1.000;
constexpr double kBlameWindowSeconds = 1.250;
constexpr double kSourceDropRatio = 0.90;
constexpr double kRecoveryRatio = 0.97;
constexpr double kSuccessfulProbeHoldSeconds = 5.0;

// If an already-established interpolation cost can no longer keep aggregate
// output near the requested target, test one cheaper level before adding work.
// The probe is retained only when it materially recovers source cadence without
// materially reducing aggregate source+generated throughput.
constexpr double kSourcePreservationOutputRatio = 0.95;
constexpr double kSourcePreservationConfirmSeconds = 0.60;
constexpr double kSourcePreservationProbeSeconds = 0.60;
constexpr double kSourcePreservationGainRatio = 1.08;
constexpr double kSourcePreservationKeepOutputRatio = 0.95;
constexpr double kSourcePreservationRetryHoldSeconds = 5.0;
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

    const auto nextSourceDeadline = [&]() {
        return sourceArrivalTimeNs
                > std::numeric_limits<uint64_t>::max() - intervalNs
            ? std::numeric_limits<uint64_t>::max()
            : sourceArrivalTimeNs + intervalNs;
    };

    if (!initialized_) {
        initialized_ = true;
        sourceIndex_ = 0;
        sample.previousSourceDesiredTimeNs = sourceArrivalTimeNs;
        sourceDesiredTimeNs_ = nextSourceDeadline();
        sample.rebased = true;
        sample.sourceDeadlineErrorNs = 0;
    } else {
        ++sourceIndex_;

        // Compare the real source against the prediction made by the previous
        // source, then anchor this interval to the real arrival. The old
        // cumulative candidate kept the very first interval as a permanent
        // phase offset; one startup/resume outlier could therefore leave an
        // otherwise-stable source tens of milliseconds ahead/behind forever.
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
            intervalNs / 4ULL,
            kMinPhaseCorrectionNs,
            kMaxPhaseCorrectionNs);

        sample.rebased = absoluteErrorNs >= materialPhaseErrorNs;
        sample.previousSourceDesiredTimeNs = sourceArrivalTimeNs;
        sourceDesiredTimeNs_ = nextSourceDeadline();
    }

    lastIntervalNs_ = intervalNs;
    sample.sourceIndex = sourceIndex_;
    sample.intervalNs = intervalNs;
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

DeadlineAdmissionDecision DeadlineAdmissionPredictor::predict(
        std::size_t generationCount, double usableBudgetMs) const {
    DeadlineAdmissionDecision decision{
        .usableBudgetMs = usableBudgetMs,
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
            <= usableBudgetMs;
    return decision;
}

void DeadlineAdmissionPredictor::reset() {
    hasEstimate_ = false;
    mipmapsMs_ = 0.0;
    opticalFlowMs_ = 0.0;
    perGeneratedMs_ = 0.0;
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
    // scene-rate inference. If this scheduler was already running, remember
    // that intent across the menu's suspend/resume discontinuity. The first
    // valid cadence sample can then seed the requested generation ceiling
    // immediately while the existing causal backoff logic watches for a source
    // FPS regression.
    reconfigureWarmStartPending_ = hadRuntimeCadence
        && wasActive
        && targetFps_ != 0
        && maxGeneratedFrames_ != 0;
}

std::size_t AdaptiveFrameScheduler::plan(std::chrono::nanoseconds sourceInterval) {
    telemetry_.sourceRateSnapped = false;
    telemetry_.costRaised = false;
    telemetry_.costBackedOff = false;
    telemetry_.costProbe = false;
    telemetry_.discontinuityReset = false;
    telemetry_.configWarmStart = false;
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
        // cost. Seed to that bounded requirement instead of spending 0.6 s per
        // level re-climbing from cost 1. Treat the seed like a normal confirmed
        // raise so the existing blame window immediately backs off if the extra
        // work materially reduces source FPS.
        const auto requiredCost = static_cast<std::size_t>(std::clamp(
            std::ceil(wantedGenerated - 1e-6),
            1.0,
            static_cast<double>(maxGeneratedFrames_)));
        costLimit_ = std::max(costLimit_, requiredCost);
        if (costLimit_ > 1) {
            pendingCostRaise_ = true;
            pendingRaiseWasProbe_ = false;
            probeAfterBackoff_ = false;
            pendingRaiseBaselineFps_ = telemetry_.smoothedSourceFps;
            pendingRaiseTimeSeconds_ = observedTimeSeconds_;
            lastCostChangeTimeSeconds_ = observedTimeSeconds_;
        }
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
    const double intervalOutputDemand =
        static_cast<double>(targetFps_) * intervalSeconds;
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

void AdaptiveFrameScheduler::resetRateChangeCandidates() {
    slowRateChangeSamples_ = 0;
    fastRateChangeSamples_ = 0;
    slowIntervalAccumulatorSeconds_ = 0.0;
    fastIntervalAccumulatorSeconds_ = 0.0;
}

void AdaptiveFrameScheduler::resetUnmetDemand() {
    unmetDemandSinceSeconds_ = -1.0;
    unmetSourceFpsSum_ = 0.0;
    unmetSourceFpsSamples_ = 0;
}

void AdaptiveFrameScheduler::updateSourceRate(double intervalSeconds) {
    telemetry_.sourceFps = 1.0 / intervalSeconds;

    if (!hasSmoothedInterval_) {
        smoothedSourceIntervalSeconds_ = intervalSeconds;
        hasSmoothedInterval_ = true;
        resetRateChangeCandidates();
    } else {
        // Classify against the estimate that existed before observing this
        // sample. Every valid source interval still contributes to the EMA;
        // the confirmation counters below control only hard baseline snaps.
        // This is important for alternating cadences such as 20/50 ms, where
        // waiting for three consecutive slow samples would otherwise discard
        // every long interval and substantially overestimate source FPS.
        const double previousSmoothedInterval = smoothedSourceIntervalSeconds_;
        const bool slowerCadence =
            intervalSeconds > previousSmoothedInterval * kSlowIntervalHigh;
        const bool fasterCadence =
            intervalSeconds < previousSmoothedInterval * kFastIntervalLow;

        if (slowerCadence) {
            // A heavier scene needs a prompt response. Three consistent slower
            // samples still trigger a hard snap, but provisional slow samples
            // now influence the EMA so mixed cadences cannot hide them.
            slowRateChangeSamples_++;
            slowIntervalAccumulatorSeconds_ += intervalSeconds;
            fastRateChangeSamples_ = 0;
            fastIntervalAccumulatorSeconds_ = 0.0;

            if (slowRateChangeSamples_ >= kSlowSamplesRequired) {
                smoothedSourceIntervalSeconds_ =
                    slowIntervalAccumulatorSeconds_
                    / static_cast<double>(slowRateChangeSamples_);
                resetRateChangeCandidates();
                resetUnmetDemand();
                raiseHoldUntilSeconds_ = std::max(
                    raiseHoldUntilSeconds_,
                    observedTimeSeconds_ + kPostRateChangeRaiseHoldSeconds);
                telemetry_.sourceRateSnapped = true;
            } else {
                smoothedSourceIntervalSeconds_ +=
                    kIntervalSmoothing * (intervalSeconds - smoothedSourceIntervalSeconds_);
            }
        } else if (fasterCadence) {
            // Android/WSI can present a handful of frames in a short burst after
            // a stall or UI transition. Requiring twice as many confirming
            // samples for a source-rate increase prevents those bursts from
            // being interpreted as a sustainable 100-300 FPS game cadence.
            // Provisional fast samples still contribute through the low-alpha
            // EMA, so legitimate mixed cadence is measured instead of frozen.
            fastRateChangeSamples_++;
            fastIntervalAccumulatorSeconds_ += intervalSeconds;
            slowRateChangeSamples_ = 0;
            slowIntervalAccumulatorSeconds_ = 0.0;

            if (fastRateChangeSamples_ >= kFastSamplesRequired) {
                smoothedSourceIntervalSeconds_ =
                    fastIntervalAccumulatorSeconds_
                    / static_cast<double>(fastRateChangeSamples_);
                resetRateChangeCandidates();
                resetUnmetDemand();
                raiseHoldUntilSeconds_ = std::max(
                    raiseHoldUntilSeconds_,
                    observedTimeSeconds_ + kPostRateChangeRaiseHoldSeconds);
                telemetry_.sourceRateSnapped = true;
            } else {
                smoothedSourceIntervalSeconds_ +=
                    kIntervalSmoothing * (intervalSeconds - smoothedSourceIntervalSeconds_);
            }
        } else {
            resetRateChangeCandidates();
            smoothedSourceIntervalSeconds_ +=
                kIntervalSmoothing * (intervalSeconds - smoothedSourceIntervalSeconds_);
        }
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

    const double sourceFps = telemetry_.smoothedSourceFps;

    // First evaluate a generation level that was already raised. A confirmed
    // source-rate collapse inside the blame window is a stronger signal than a
    // new demand calculation and should back off immediately.
    if (pendingCostRaise_) {
        const double sinceRaise = observedTimeSeconds_ - pendingRaiseTimeSeconds_;
        const bool sourceDropped = pendingRaiseBaselineFps_ > 0.0
            && sourceFps < pendingRaiseBaselineFps_ * kSourceDropRatio;

        if (sinceRaise <= kBlameWindowSeconds && sourceDropped) {
            if (costLimit_ > 1)
                costLimit_--;
            pendingCostRaise_ = false;
            probeAfterBackoff_ = true;
            pendingRaiseWasProbe_ = false;
            lastBackoffTimeSeconds_ = observedTimeSeconds_;
            resetUnmetDemand();
            telemetry_.costBackedOff = true;
            return;
        }

        if (sinceRaise >= kBlameWindowSeconds) {
            const bool completedProbe = pendingRaiseWasProbe_;
            pendingCostRaise_ = false;
            pendingRaiseWasProbe_ = false;
            if (completedProbe)
                successfulProbeHoldUntilSeconds_ =
                    observedTimeSeconds_ + kSuccessfulProbeHoldSeconds;
        }
    }

    if (sourcePreservationProbeActive_) {
        sourcePreservationProbeFpsSum_ += sourceFps;
        sourcePreservationProbeSamples_++;

        if (observedTimeSeconds_ - sourcePreservationProbeStartedSeconds_
                >= kSourcePreservationProbeSeconds) {
            // sourceFps is already the scheduler's smoothed, burst-filtered
            // cadence estimate. Averaging the whole probe again would include
            // the intentional recovery ramp and understate the lower level.
            const double recoveredSourceFps = sourceFps;
            const double originalOutputFps = sourcePreservationBaselineFps_
                * static_cast<double>(sourcePreservationOriginalCost_ + 1);
            const double probedOutputFps = recoveredSourceFps
                * static_cast<double>(costLimit_ + 1);
            const bool sourceRecovered = sourcePreservationBaselineFps_ > 0.0
                && recoveredSourceFps
                    >= sourcePreservationBaselineFps_ * kSourcePreservationGainRatio;
            const bool throughputPreserved = originalOutputFps <= 0.0
                || probedOutputFps
                    >= originalOutputFps * kSourcePreservationKeepOutputRatio;
            const bool targetNearlyMet = targetFps_ > 0
                && probedOutputFps
                    >= static_cast<double>(targetFps_) * kSourcePreservationOutputRatio;

            sourcePreservationProbeActive_ = false;
            sourcePreservationProbeFpsSum_ = 0.0;
            sourcePreservationProbeSamples_ = 0;

            if (sourceRecovered && (throughputPreserved || targetNearlyMet)) {
                // Require the cheaper level to recover almost enough source FPS
                // to satisfy the target before a later upward probe is allowed.
                pendingRaiseBaselineFps_ = targetFps_ > 0
                    ? static_cast<double>(targetFps_)
                        / static_cast<double>(costLimit_ + 1)
                    : recoveredSourceFps;
                probeAfterBackoff_ = true;
                lastBackoffTimeSeconds_ = observedTimeSeconds_;
                successfulProbeHoldUntilSeconds_ =
                    observedTimeSeconds_ + kSuccessfulProbeHoldSeconds;
                telemetry_.costProbe = true;
            } else {
                costLimit_ = std::min(
                    sourcePreservationOriginalCost_, maxGeneratedFrames_);
                probeAfterBackoff_ = false;
                lastCostChangeTimeSeconds_ = observedTimeSeconds_;
                raiseHoldUntilSeconds_ =
                    observedTimeSeconds_ + kSuccessfulProbeHoldSeconds;
                sourcePreservationProbeHoldUntilSeconds_ =
                    observedTimeSeconds_ + kSourcePreservationRetryHoldSeconds;
                telemetry_.costRaised = true;
                telemetry_.costProbe = true;
            }

            resetUnmetDemand();
        }
        return;
    }

    const double projectedOutputFps =
        sourceFps * static_cast<double>(costLimit_ + 1);
    const bool sourceStarvedAtCurrentCost =
        costLimit_ > 1
        && targetFps_ > 0
        && projectedOutputFps
            < static_cast<double>(targetFps_) * kSourcePreservationOutputRatio;

    if (sourceStarvedAtCurrentCost
            && observedTimeSeconds_ >= sourcePreservationProbeHoldUntilSeconds_) {
        if (sourcePreservationSinceSeconds_ < 0.0) {
            sourcePreservationSinceSeconds_ = observedTimeSeconds_;
            sourcePreservationFpsSum_ = sourceFps;
            sourcePreservationSamples_ = 1;
        } else {
            sourcePreservationFpsSum_ += sourceFps;
            sourcePreservationSamples_++;
        }

        if (observedTimeSeconds_ - sourcePreservationSinceSeconds_
                >= kSourcePreservationConfirmSeconds) {
            sourcePreservationOriginalCost_ = costLimit_;
            sourcePreservationBaselineFps_ = sourcePreservationSamples_ > 0
                ? sourcePreservationFpsSum_
                    / static_cast<double>(sourcePreservationSamples_)
                : sourceFps;
            costLimit_--;
            sourcePreservationProbeActive_ = true;
            sourcePreservationProbeStartedSeconds_ = observedTimeSeconds_;
            sourcePreservationProbeFpsSum_ = 0.0;
            sourcePreservationProbeSamples_ = 0;
            sourcePreservationSinceSeconds_ = -1.0;
            sourcePreservationFpsSum_ = 0.0;
            sourcePreservationSamples_ = 0;
            lastCostChangeTimeSeconds_ = observedTimeSeconds_;
            resetUnmetDemand();
            telemetry_.costBackedOff = true;
            telemetry_.costProbe = true;
            return;
        }

        // Do not let the ordinary unmet-demand path race the protective
        // measurement by raising generation cost while starvation evidence is
        // still being confirmed.
        resetUnmetDemand();
        return;
    } else {
        sourcePreservationSinceSeconds_ = -1.0;
        sourcePreservationFpsSum_ = 0.0;
        sourcePreservationSamples_ = 0;
    }

    if (costLimit_ >= maxGeneratedFrames_) {
        resetUnmetDemand();
        return;
    }

    if (wantedGeneratedFrames <= static_cast<double>(costLimit_) + 0.001) {
        resetUnmetDemand();
        return;
    }

    // Observe a persistent deficit before adding more GPU work. This replaces
    // the old 250 ms raise cadence, which could repeatedly climb during short
    // timing disturbances. The average source rate gathered during this window
    // becomes the pre-raise causal baseline.
    if (unmetDemandSinceSeconds_ < 0.0) {
        unmetDemandSinceSeconds_ = observedTimeSeconds_;
        unmetSourceFpsSum_ = sourceFps;
        unmetSourceFpsSamples_ = 1;
        return;
    }

    unmetSourceFpsSum_ += sourceFps;
    unmetSourceFpsSamples_++;

    if (observedTimeSeconds_ - unmetDemandSinceSeconds_ < kSustainedDemandSeconds)
        return;
    if (pendingCostRaise_)
        return;
    if (observedTimeSeconds_ < successfulProbeHoldUntilSeconds_)
        return;
    if (observedTimeSeconds_ < raiseHoldUntilSeconds_)
        return;

    const double baselineSourceFps = unmetSourceFpsSamples_ > 0
        ? unmetSourceFpsSum_ / static_cast<double>(unmetSourceFpsSamples_)
        : sourceFps;

    if (probeAfterBackoff_) {
        if (lastBackoffTimeSeconds_ < 0.0
                || observedTimeSeconds_ - lastBackoffTimeSeconds_ < kProbeIntervalSeconds)
            return;
        if (pendingRaiseBaselineFps_ > 0.0
                && sourceFps < pendingRaiseBaselineFps_ * kRecoveryRatio)
            return;

        costLimit_++;
        pendingCostRaise_ = true;
        pendingRaiseWasProbe_ = true;
        probeAfterBackoff_ = false;
        pendingRaiseBaselineFps_ = baselineSourceFps;
        pendingRaiseTimeSeconds_ = observedTimeSeconds_;
        lastCostChangeTimeSeconds_ = observedTimeSeconds_;
        resetUnmetDemand();
        telemetry_.costRaised = true;
        telemetry_.costProbe = true;
        return;
    }

    costLimit_++;
    pendingCostRaise_ = true;
    pendingRaiseWasProbe_ = false;
    pendingRaiseBaselineFps_ = baselineSourceFps;
    pendingRaiseTimeSeconds_ = observedTimeSeconds_;
    lastCostChangeTimeSeconds_ = observedTimeSeconds_;
    resetUnmetDemand();
    telemetry_.costRaised = true;
}

void AdaptiveFrameScheduler::resetRuntimeState() {
    fractionalOpportunityPhase_ = 0.0;
    smoothedSourceIntervalSeconds_ = 0.0;
    hasSmoothedInterval_ = false;
    reconfigureWarmStartPending_ = false;
    resetRateChangeCandidates();
    observedTimeSeconds_ = 0.0;
    costLimit_ = maxGeneratedFrames_ == 0 ? 0 : 1;
    pendingCostRaise_ = false;
    probeAfterBackoff_ = false;
    pendingRaiseWasProbe_ = false;
    pendingRaiseBaselineFps_ = 0.0;
    pendingRaiseTimeSeconds_ = 0.0;
    lastCostChangeTimeSeconds_ = -1.0;
    lastBackoffTimeSeconds_ = -1.0;
    successfulProbeHoldUntilSeconds_ = 0.0;
    raiseHoldUntilSeconds_ = 0.0;
    sourcePreservationProbeHoldUntilSeconds_ = 0.0;
    resetUnmetDemand();
    telemetry_ = {};
    telemetry_.costLimit = costLimit_;
}

void AdaptiveFrameScheduler::reset() {
    runtimeCadenceEstablished_ = false;
    lastTrustedSourceIntervalSeconds_ = 0.0;
    resetRuntimeState();
}
