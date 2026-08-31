#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

/// Chooses the minimum number of interpolation frames needed to approach an
/// output FPS ceiling. It owns no Vulkan objects and is independently testable.
class AdaptiveFrameScheduler {
public:
    using Clock = std::chrono::steady_clock;

    AdaptiveFrameScheduler() = default;
    AdaptiveFrameScheduler(uint32_t targetFps, std::size_t maxGeneratedFrames);

    /// Apply a hot-reloaded target. A changed target clears fractional and
    /// smoothing state so the previous cap cannot leak into the new schedule.
    void configure(uint32_t targetFps, std::size_t maxGeneratedFrames);

    /// Observe the best available source-frame interval and return the minimum
    /// useful generation count in the range 0..maxGeneratedFrames.
    std::size_t plan(std::chrono::nanoseconds sourceInterval);

    /// Reserve the next source-only output slot. Generated frames are emitted
    /// in their source interval; pacing only a source-only cycle prevents a
    /// fast game from exceeding the cap without slowing an already-low base FPS.
    std::chrono::nanoseconds delayUntilNextSourceOutput(Clock::time_point now);

    void reset();

    [[nodiscard]] uint32_t targetFps() const { return targetFps_; }

private:
    uint32_t targetFps_{};
    std::size_t maxGeneratedFrames_{};
    double fractionalGeneratedBudget_{};
    double smoothedSourceIntervalSeconds_{};
    bool hasSmoothedInterval_{false};
    Clock::time_point nextSourceOutputSlot_{};
    bool hasSourceOutputSlot_{false};
};
