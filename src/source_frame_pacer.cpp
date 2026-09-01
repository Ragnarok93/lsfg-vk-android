#include "source_frame_pacer.hpp"

void SourceFramePacer::configure(uint32_t targetFps) noexcept {
    if (targetFps_ == targetFps)
        return;
    targetFps_ = targetFps;
    nextDeadline_.reset();
}

std::chrono::nanoseconds SourceFramePacer::delayUntilNext(Clock::time_point) noexcept {
    return std::chrono::nanoseconds::zero();
}
