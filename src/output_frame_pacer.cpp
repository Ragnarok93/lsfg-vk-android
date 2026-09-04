#include "output_frame_pacer.hpp"

void OutputFramePacer::configure(uint32_t targetFps) noexcept {
    pacer_.configure(targetFps);
}

std::chrono::nanoseconds OutputFramePacer::delayUntilNext(Clock::time_point now) noexcept {
    return pacer_.delayUntilNext(now);
}
