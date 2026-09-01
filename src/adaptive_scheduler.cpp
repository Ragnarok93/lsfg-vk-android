#include "adaptive_scheduler.hpp"

#include <algorithm>
#include <cmath>

AdaptiveFrameScheduler::AdaptiveFrameScheduler(
        uint32_t targetFps, std::size_t maxGeneratedFrames)
        : targetFps_(targetFps),
          maxGeneratedFrames_(maxGeneratedFrames) {}

void AdaptiveFrameScheduler::configure(
        uint32_t targetFps, std::size_t maxGeneratedFrames) {
    if (targetFps_ == targetFps && maxGeneratedFrames_ == maxGeneratedFrames)
        return;
    targetFps_ = targetFps;
    maxGeneratedFrames_ = maxGeneratedFrames;
    reset();
}

std::size_t AdaptiveFrameScheduler::plan(std::chrono::nanoseconds sourceInterval) {
    if (targetFps_ == 0 || maxGeneratedFrames_ == 0)
        return 0;

    const double intervalSeconds = std::chrono::duration<double>(sourceInterval).count();
    if (!(intervalSeconds > 0.0) || !std::isfinite(intervalSeconds))
        return 0;

    // A pause, app switch, or shader-compilation stall is not a useful signal
    // for interpolation. Drop the old budget instead of emitting a burst.
    if (intervalSeconds >= 0.250) {
        reset();
        return 0;
    }

    // Damp one-frame timing spikes while still following sustained FPS changes.
    constexpr double smoothing = 0.25;
    if (!hasSmoothedInterval_) {
        smoothedSourceIntervalSeconds_ = intervalSeconds;
        hasSmoothedInterval_ = true;
    } else {
        smoothedSourceIntervalSeconds_ +=
            smoothing * (intervalSeconds - smoothedSourceIntervalSeconds_);
    }

    // A source frame already occupies one output slot. Carry the fractional
    // remainder so ratios such as 50 -> 60 distribute one generated frame over
    // several source frames instead of forcing a fixed multiplier.
    const double wantedGenerated = std::clamp(
        static_cast<double>(targetFps_) * smoothedSourceIntervalSeconds_ - 1.0,
        0.0,
        static_cast<double>(maxGeneratedFrames_));
    fractionalGeneratedBudget_ += wantedGenerated;

    const auto generated = static_cast<std::size_t>(
        std::floor(fractionalGeneratedBudget_ + 1e-6));
    const auto clamped = std::min(generated, maxGeneratedFrames_);
    fractionalGeneratedBudget_ -= static_cast<double>(clamped);

    // Do not carry a saturated stall budget into following healthy frames.
    if (clamped == maxGeneratedFrames_)
        fractionalGeneratedBudget_ = std::min(fractionalGeneratedBudget_, 0.999999);
    return clamped;
}

std::chrono::nanoseconds AdaptiveFrameScheduler::delayUntilNextSourceOutput(
        Clock::time_point now) {
    if (targetFps_ == 0)
        return std::chrono::nanoseconds::zero();

    const auto period = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::duration<double>(1.0 / static_cast<double>(targetFps_)));
    if (!hasSourceOutputSlot_ || now >= nextSourceOutputSlot_) {
        hasSourceOutputSlot_ = true;
        nextSourceOutputSlot_ = now + period;
        return std::chrono::nanoseconds::zero();
    }

    const auto delay = std::chrono::duration_cast<std::chrono::nanoseconds>(
        nextSourceOutputSlot_ - now);
    nextSourceOutputSlot_ += period;
    return delay;
}

void AdaptiveFrameScheduler::reset() {
    fractionalGeneratedBudget_ = 0.0;
    smoothedSourceIntervalSeconds_ = 0.0;
    hasSmoothedInterval_ = false;
    hasSourceOutputSlot_ = false;
}
