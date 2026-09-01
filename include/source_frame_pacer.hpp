#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

class SourceFramePacer {
public:
    using Clock = std::chrono::steady_clock;

    void configure(uint32_t targetFps) noexcept;
    std::chrono::nanoseconds delayUntilNext(Clock::time_point now) noexcept;
    uint32_t targetFps() const noexcept { return targetFps_; }

private:
    uint32_t targetFps_{0};
    std::optional<Clock::time_point> nextDeadline_{};
};
