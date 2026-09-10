#include "fixed_frame_governor.hpp"

#include <algorithm>
#include <cmath>

namespace {
constexpr double kIntervalSmoothing = 0.15;
constexpr double kSlowIntervalHigh = 1.40;
constexpr double kFastIntervalLow = 0.70;
constexpr unsigned kSlowSamplesRequired = 3;
constexpr unsigned kFastSamplesRequired = 6;
constexpr double kDiscontinuitySeconds = 0.250;
constexpr double kStableBeforeRaiseSeconds = 0.750;
constexpr double kRaiseBlameWindowSeconds = 1.250;
constexpr double kSourceDropRatio = 0.90;
constexpr double kBackoffHoldSeconds = 5.0;
constexpr double kRecoveryRatio = 0.97;
constexpr double kRefreshRaiseHeadroom = 0.98;
} // namespace

void FixedFrameGovernor::configure(bool enabled, std::size_t requestedMultiplier,
        uint32_t displayRefreshHz) {
    const std::size_t requestedGenerated = requestedMultiplier > 1
        ? std::min<std::size_t>(requestedMultiplier - 1, 3)
        : 0;
    if (enabled_ == enabled
            && requestedGeneratedFrames_ == requestedGenerated
            && displayRefreshHz_ == displayRefreshHz)
        return;

    enabled_ = enabled;
    requestedGeneratedFrames_ = requestedGenerated;
    displayRefreshHz_ = displayRefreshHz;
    resetMeasurements(true);
}

std::size_t FixedFrameGovernor::plan(std::chrono::nanoseconds sourceInterval) {
    telemetry_.sourceRateSnapped = false;
    telemetry_.costRaised = false;
    telemetry_.costBackedOff = false;
    telemetry_.costProbe = false;
    telemetry_.refreshLimited = false;
    telemetry_.discontinuityReset = false;
    telemetry_.requestedGeneratedFrames = requestedGeneratedFrames_;

    if (requestedGeneratedFrames_ == 0) {
        telemetry_.costLimit = 0;
        telemetry_.generatedFrames = 0;
        return 0;
    }

    if (!enabled_) {
        telemetry_.costLimit = requestedGeneratedFrames_;
        telemetry_.generatedFrames = requestedGeneratedFrames_;
        return requestedGeneratedFrames_;
    }

    costLimit_ = std::clamp<std::size_t>(costLimit_, 1, requestedGeneratedFrames_);
    const double intervalSeconds = std::chrono::duration<double>(sourceInterval).count();
    if (!(intervalSeconds > 0.0) || !std::isfinite(intervalSeconds)) {
        const auto output = safeOutput();
        telemetry_.costLimit = costLimit_;
        telemetry_.generatedFrames = output;
        return output;
    }

    if (intervalSeconds >= kDiscontinuitySeconds) {
        resetMeasurements(false);
        telemetry_.discontinuityReset = true;
        const auto output = safeOutput();
        telemetry_.costLimit = costLimit_;
        telemetry_.generatedFrames = output;
        return output;
    }

    observedSeconds_ += intervalSeconds;
    const bool rateSnapped = updateSourceRate(intervalSeconds);
    telemetry_.sourceRateSnapped = rateSnapped;
    const double sourceFps = telemetry_.smoothedSourceFps;

    if (pendingRaise_) {
        const double elapsed = observedSeconds_ - pendingRaiseSeconds_;
        const bool sourceDropped = pendingRaiseBaselineFps_ > 0.0
            && sourceFps < pendingRaiseBaselineFps_ * kSourceDropRatio;
        if (elapsed <= kRaiseBlameWindowSeconds && sourceDropped) {
            if (costLimit_ > 1)
                --costLimit_;
            pendingRaise_ = false;
            probeAfterBackoff_ = true;
            pendingRaiseWasProbe_ = false;
            holdUntilSeconds_ = observedSeconds_ + kBackoffHoldSeconds;
            stableSinceSeconds_ = observedSeconds_;
            telemetry_.costBackedOff = true;
        } else if (elapsed >= kRaiseBlameWindowSeconds) {
            pendingRaise_ = false;
            pendingRaiseWasProbe_ = false;
            stableSinceSeconds_ = observedSeconds_;
        }
    }

    const std::size_t refreshCeiling = refreshGenerationCeiling(sourceFps);
    if (refreshCeiling < costLimit_) {
        costLimit_ = std::max<std::size_t>(1, refreshCeiling);
        pendingRaise_ = false;
        pendingRaiseWasProbe_ = false;
        stableSinceSeconds_ = observedSeconds_;
        telemetry_.refreshLimited = true;
    }

    if (rateSnapped)
        stableSinceSeconds_ = observedSeconds_;

    if (costLimit_ < requestedGeneratedFrames_
            && !pendingRaise_
            && observedSeconds_ >= holdUntilSeconds_
            && observedSeconds_ - stableSinceSeconds_ >= kStableBeforeRaiseSeconds) {
        const std::size_t candidate = costLimit_ + 1;
        const bool refreshAllows = displayRefreshHz_ == 0 || sourceFps <= 0.0
            || sourceFps * static_cast<double>(candidate + 1)
                <= static_cast<double>(displayRefreshHz_) * kRefreshRaiseHeadroom;
        const bool recovered = !probeAfterBackoff_
            || pendingRaiseBaselineFps_ <= 0.0
            || sourceFps >= pendingRaiseBaselineFps_ * kRecoveryRatio;
        if (refreshAllows && recovered) {
            ++costLimit_;
            pendingRaise_ = true;
            pendingRaiseWasProbe_ = probeAfterBackoff_;
            probeAfterBackoff_ = false;
            pendingRaiseBaselineFps_ = sourceFps;
            pendingRaiseSeconds_ = observedSeconds_;
            telemetry_.costRaised = true;
            telemetry_.costProbe = pendingRaiseWasProbe_;
        } else if (!refreshAllows) {
            telemetry_.refreshLimited = true;
        }
    }

    const auto output = safeOutput();
    telemetry_.costLimit = costLimit_;
    telemetry_.generatedFrames = output;
    return output;
}

void FixedFrameGovernor::resetRateCandidates() {
    slowRateSamples_ = 0;
    fastRateSamples_ = 0;
    slowIntervalSum_ = 0.0;
    fastIntervalSum_ = 0.0;
}

bool FixedFrameGovernor::updateSourceRate(double intervalSeconds) {
    telemetry_.sourceFps = 1.0 / intervalSeconds;
    bool snapped = false;
    if (!hasSmoothedInterval_) {
        smoothedSourceIntervalSeconds_ = intervalSeconds;
        hasSmoothedInterval_ = true;
        stableSinceSeconds_ = observedSeconds_;
        resetRateCandidates();
    } else if (intervalSeconds > smoothedSourceIntervalSeconds_ * kSlowIntervalHigh) {
        ++slowRateSamples_;
        slowIntervalSum_ += intervalSeconds;
        fastRateSamples_ = 0;
        fastIntervalSum_ = 0.0;
        if (slowRateSamples_ >= kSlowSamplesRequired) {
            smoothedSourceIntervalSeconds_ = slowIntervalSum_ / slowRateSamples_;
            resetRateCandidates();
            snapped = true;
        }
    } else if (intervalSeconds < smoothedSourceIntervalSeconds_ * kFastIntervalLow) {
        ++fastRateSamples_;
        fastIntervalSum_ += intervalSeconds;
        slowRateSamples_ = 0;
        slowIntervalSum_ = 0.0;
        if (fastRateSamples_ >= kFastSamplesRequired) {
            smoothedSourceIntervalSeconds_ = fastIntervalSum_ / fastRateSamples_;
            resetRateCandidates();
            snapped = true;
        }
    } else {
        resetRateCandidates();
        smoothedSourceIntervalSeconds_ +=
            kIntervalSmoothing * (intervalSeconds - smoothedSourceIntervalSeconds_);
    }

    telemetry_.smoothedSourceFps = smoothedSourceIntervalSeconds_ > 0.0
        ? 1.0 / smoothedSourceIntervalSeconds_
        : 0.0;
    return snapped;
}

std::size_t FixedFrameGovernor::refreshGenerationCeiling(double sourceFps) const {
    if (displayRefreshHz_ == 0 || !(sourceFps > 0.0))
        return requestedGeneratedFrames_;

    const double slots = static_cast<double>(displayRefreshHz_) / sourceFps;
    const auto wholeSlots = static_cast<std::size_t>(std::floor(slots + 1e-6));
    if (wholeSlots <= 1)
        return 1;
    return std::clamp<std::size_t>(wholeSlots - 1, 1, requestedGeneratedFrames_);
}

std::size_t FixedFrameGovernor::safeOutput() const {
    if (requestedGeneratedFrames_ == 0)
        return 0;
    return std::clamp<std::size_t>(costLimit_, 1, requestedGeneratedFrames_);
}

void FixedFrameGovernor::resetMeasurements(bool resetCostLimit) {
    smoothedSourceIntervalSeconds_ = 0.0;
    hasSmoothedInterval_ = false;
    resetRateCandidates();
    observedSeconds_ = 0.0;
    stableSinceSeconds_ = 0.0;
    pendingRaise_ = false;
    probeAfterBackoff_ = false;
    pendingRaiseWasProbe_ = false;
    pendingRaiseBaselineFps_ = 0.0;
    pendingRaiseSeconds_ = 0.0;
    holdUntilSeconds_ = 0.0;
    if (resetCostLimit)
        costLimit_ = requestedGeneratedFrames_ == 0 ? 0 : 1;
    telemetry_ = {};
    telemetry_.requestedGeneratedFrames = requestedGeneratedFrames_;
    telemetry_.costLimit = costLimit_;
    telemetry_.generatedFrames = safeOutput();
}

void FixedFrameGovernor::reset() {
    resetMeasurements(true);
}
