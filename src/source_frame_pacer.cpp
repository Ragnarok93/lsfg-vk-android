#include "source_frame_pacer.hpp"

void SourceFramePacer::configure(uint32_t targetFps) noexcept {
    pacer_.configure(targetFps);
}

std::chrono::nanoseconds SourceFramePacer::delayUntilNext(Clock::time_point now) noexcept {
    return pacer_.delayUntilNext(now);
}
