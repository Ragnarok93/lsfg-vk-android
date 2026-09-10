#include "adaptive_scheduler.hpp"

#include <algorithm>
#include <cmath>

namespace {
constexpr double kIntervalSmoothing = 0.15;
constexpr double kRapidIntervalHigh = 1.40;
constexpr double kRapidIntervalLow = 0.70;
constexpr unsigned kRapidSamplesRequired = 3;
constexpr double kDiscontinuitySeconds = 0.250;

constexpr double kRaiseIntervalSeconds = 0.250;
constexpr double kProbeIntervalSeconds = 0.500;
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

    // A pause, app switch, or shader-compilation stall is not a useful source
    // cadence sample. Reset controller state rather than accumulating output
    // debt or blaming frame generation for a discontinuity.
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

void AdaptiveFrameScheduler::updateSourceRate(double intervalSeconds) {
    telemetry_.sourceFps = 1.0 / intervalSeconds;

    if (!hasSmoothedInterval_) {
        smoothedSourceIntervalSeconds_ = intervalSeconds;
        hasSmoothedInterval_ = true;
        rapidRateChangeSamples_ = 0;
    } else {
        const bool rapidChange =
            intervalSeconds > smoothedSourceIntervalSeconds_ * kRapidIntervalHigh
            || intervalSeconds < smoothedSourceIntervalSeconds_ * kRapidIntervalLow;

        if (rapidChange) {
            rapidRateChangeSamples_++;
            if (rapidRateChangeSamples_ >= kRapidSamplesRequired) {
                smoothedSourceIntervalSeconds_ = intervalSeconds;
                rapidRateChangeSamples_ = 0;
                telemetry_.sourceRateSnapped = true;
            }
        } else {
            rapidRateChangeSamples_ = 0;
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
        return;
    }

    if (costLimit_ == 0)
        costLimit_ = 1;
    costLimit_ = std::min(costLimit_, maxGeneratedFrames_);

    const double sourceFps = telemetry_.smoothedSourceFps;

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

    if (costLimit_ >= maxGeneratedFrames_)
        return;
    if (wantedGeneratedFrames <= static_cast<double>(costLimit_) + 0.001)
        return;
    if (pendingCostRaise_)
        return;
    if (observedTimeSeconds_ < successfulProbeHoldUntilSeconds_)
        return;

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
        pendingRaiseBaselineFps_ = sourceFps;
        pendingRaiseTimeSeconds_ = observedTimeSeconds_;
        lastCostChangeTimeSeconds_ = observedTimeSeconds_;
        telemetry_.costRaised = true;
        telemetry_.costProbe = true;
        return;
    }

    if (lastCostChangeTimeSeconds_ >= 0.0
            && observedTimeSeconds_ - lastCostChangeTimeSeconds_ < kRaiseIntervalSeconds)
        return;
    if (observedTimeSeconds_ < kRaiseIntervalSeconds)
        return;

    costLimit_++;
    pendingCostRaise_ = true;
    pendingRaiseWasProbe_ = false;
    pendingRaiseBaselineFps_ = sourceFps;
    pendingRaiseTimeSeconds_ = observedTimeSeconds_;
    lastCostChangeTimeSeconds_ = observedTimeSeconds_;
    telemetry_.costRaised = true;
}

void AdaptiveFrameScheduler::resetRuntimeState() {
    fractionalGeneratedBudget_ = 0.0;
    smoothedSourceIntervalSeconds_ = 0.0;
    hasSmoothedInterval_ = false;
    rapidRateChangeSamples_ = 0;
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
    telemetry_ = {};
    telemetry_.costLimit = costLimit_;
}

void AdaptiveFrameScheduler::reset() {
    resetRuntimeState();
}
