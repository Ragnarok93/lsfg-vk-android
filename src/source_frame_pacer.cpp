#include "source_frame_pacer.hpp"

void SourceFramePacer::configure(uint32_t targetFps) noexcept {
    if (targetFps_ == targetFps)
        return;
    targetFps_ = targetFps;
    nextDeadline_.reset();
}

std::chrono::nanoseconds SourceFramePacer::delayUntilNext(Clock::time_point now) noexcept {
    if (targetFps_ == 0) {
        nextDeadline_.reset();
        return std::chrono::nanoseconds::zero();
    }

    const auto period = std::chrono::nanoseconds(
        1'000'000'000ULL / static_cast<uint64_t>(targetFps_));

    if (!nextDeadline_.has_value()) {
        nextDeadline_ = now + period;
        return std::chrono::nanoseconds::zero();
    }

    if (now >= *nextDeadline_) {
        nextDeadline_ = now + period;
        return std::chrono::nanoseconds::zero();
    }

    const auto delay = std::chrono::duration_cast<std::chrono::nanoseconds>(
        *nextDeadline_ - now);
    *nextDeadline_ += period;
    return delay;
}
