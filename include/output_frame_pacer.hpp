#pragma once

#include "frame_pacer.hpp"

#include <chrono>
#include <cstdint>

// Spaces the generated/source images that make up one LSFG output stream.
// Mailbox accepts back-to-back presents but may replace all intermediary
// images before scanout, so successful vkQueuePresentKHR calls alone are not
// sufficient evidence of visible frame generation.
class OutputFramePacer {
public:
    using Clock = FramePacer::Clock;

    void configure(uint32_t targetFps) noexcept;
    std::chrono::nanoseconds delayUntilNext(Clock::time_point now) noexcept;

private:
    FramePacer pacer_;
};
