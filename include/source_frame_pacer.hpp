#pragma once

#include "frame_pacer.hpp"

#include <chrono>
#include <cstdint>

class SourceFramePacer {
public:
    using Clock = FramePacer::Clock;

    void configure(uint32_t targetFps) noexcept;
    std::chrono::nanoseconds delayUntilNext(Clock::time_point now) noexcept;
    uint32_t targetFps() const noexcept { return pacer_.targetFps(); }

private:
    FramePacer pacer_;
};
