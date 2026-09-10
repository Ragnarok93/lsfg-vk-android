#include "adaptive_scheduler.hpp"

#include <algorithm>
#include <cmath>

namespace {
constexpr double kIntervalSmoothing = 0.15;
constexpr double kSlowIntervalHigh = 1.40;
constexpr double kFastIntervalLow = 0.70;
constexpr unsigned kSlowSamplesRequired = 3;
constexpr unsigned kFastSamplesRequired = 6;
constexpr double kDiscontinuitySeconds = 0.250;

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
} // namespace

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
    targetFps_ = targetFps;
    maxGeneratedFrames_ = maxGeneratedFrames;
    resetRuntimeState();
}

std::size_t AdaptiveFrameScheduler::plan(std::chrono::nanoseconds sourceInterval) {
    telemetry_.sourceRateSnapped = false;
    telemetry_.costRaised = false;
    telemetry_.costBackedOff = false;
    telemetry_.costProbe = false;
    telemetry_.discontinuityReset = false;
    telemetry_.generatedFrames = 0;
    telemetry_.wantedGeneratedFrames = 0.0;

    if (targetFps_ == 0 || maxGeneratedFrames_ == 0) {
        telemetry_.costLimit = 0;
        return 0;
    }

    const double intervalSeconds = std::chrono::duration<double>(sourceInterval).count();
    if (!(intervalSeconds > 0.0) || !std::isfinite(intervalSeconds))
        return 0;

    // A pause, app switch, shader-compilation stall, or Quick Menu suspension
    // is not a useful source cadence sample. Reset controller state rather than
    // accumulating output debt or blaming frame generation for a discontinuity.
    if (intervalSeconds >= kDiscontinuitySeconds) {
        resetRuntimeState();
        telemetry_.discontinuityReset = true;
        return 0;
    }

    observedTimeSeconds_ += intervalSeconds;
    updateSourceRate(intervalSeconds);

    const double wantedGenerated = std::clamp(
        static_cast<double>(targetFps_) * smoothedSourceIntervalSeconds_ - 1.0,
        0.0,
        static_cast<double>(maxGeneratedFrames_));
    telemetry_.wantedGeneratedFrames = wantedGenerated;

    updateCostLimit(wantedGenerated);
    telemetry_.costLimit = costLimit_;

    // Apply the cost ceiling before fractional accumulation. This prevents an
    // intentionally suppressed high-cost request from building a backlog that
    // would burst as soon as the governor probes a higher level.
    const double governedWanted = std::min(
        wantedGenerated, static_cast<double>(costLimit_));
    fractionalGeneratedBudget_ += governedWanted;

    const auto generated = static_cast<std::size_t>(
        std::floor(fractionalGeneratedBudget_ + 1e-6));
    const auto clamped = std::min(generated, costLimit_);
    fractionalGeneratedBudget_ -= static_cast<double>(clamped);

    if (clamped == costLimit_ && costLimit_ > 0)
        fractionalGeneratedBudget_ = std::min(fractionalGeneratedBudget_, 0.999999);

    telemetry_.generatedFrames = clamped;
    return clamped;
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
        const bool slowerCadence =
            intervalSeconds > smoothedSourceIntervalSeconds_ * kSlowIntervalHigh;
        const bool fasterCadence =
            intervalSeconds < smoothedSourceIntervalSeconds_ * kFastIntervalLow;

        if (slowerCadence) {
            // A heavier scene needs a prompt response. Three consistent slower
            // samples are enough, but snap to their mean rather than the last
            // interval so one outlier cannot dominate the new baseline.
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
            }
        } else if (fasterCadence) {
            // Android/WSI can present a handful of frames in a short burst after
            // a stall or UI transition. Requiring twice as many confirming
            // samples for a source-rate increase prevents those bursts from
            // being interpreted as a sustainable 100-300 FPS game cadence.
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
    fractionalGeneratedBudget_ = 0.0;
    smoothedSourceIntervalSeconds_ = 0.0;
    hasSmoothedInterval_ = false;
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
    resetUnmetDemand();
    telemetry_ = {};
    telemetry_.costLimit = costLimit_;
}

void AdaptiveFrameScheduler::reset() {
    resetRuntimeState();
}
