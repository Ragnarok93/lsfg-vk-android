#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

/// Stateful adaptive frame-generation policy. The target is an output objective,
/// never a source-frame limiter. Vulkan presentation remains owned by LsContext.
class AdaptiveFrameScheduler {
public:
    using Clock = std::chrono::steady_clock;

    AdaptiveFrameScheduler() = default;
    AdaptiveFrameScheduler(uint32_t targetFps, std::size_t maxGeneratedFrames);

    /// Apply a hot-reloaded output target. Changed settings clear fractional,
    /// baseline, probe and cooldown state so the old policy cannot leak through.
    void configure(uint32_t targetFps, std::size_t maxGeneratedFrames);

    /// Observe the source interval produced after the previous plan and return
    /// the generated-frame count for the next source interval.
    std::size_t plan(std::chrono::nanoseconds sourceInterval);

    /// Compatibility shim for the existing Android present path. Adaptive FPS
    /// is now an objective rather than a source limiter, so this always returns
    /// zero and never intentionally sleeps the game's real-frame presentation.
    std::chrono::nanoseconds delayUntilNextSourceOutput(Clock::time_point now);

    void reset();

    [[nodiscard]] uint32_t targetFps() const { return targetFps_; }
    [[nodiscard]] std::size_t generationCeiling() const { return generationCeiling_; }
    [[nodiscard]] std::size_t provenGenerationCeiling() const {
        return provenGenerationCeiling_;
    }

private:
    struct LevelStats {
        double sourceFps{};
        std::size_t samples{};
    };

    void recordSourceSample(std::size_t generationLevel, double sourceFps);
    [[nodiscard]] bool shouldRejectProbe(std::size_t probeLevel) const;

    uint32_t targetFps_{};
    std::size_t maxGeneratedFrames_{};
    double fractionalGeneratedBudget_{};
    double smoothedSourceIntervalSeconds_{};
    bool hasSmoothedInterval_{false};

    std::size_t generationCeiling_{};
    std::size_t provenGenerationCeiling_{};
    std::size_t lastGeneratedFrameCount_{};
    std::size_t warmupSamplesRemaining_{};
    std::size_t stableDeficitSamples_{};
    std::size_t probeCooldownSamples_{};
    std::vector<LevelStats> levelStats_{};
};
