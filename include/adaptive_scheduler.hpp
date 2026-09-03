#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

/// Stateful adaptive frame-generation policy. The target is an output objective,
/// never a source-frame limiter. Vulkan presentation remains owned by LsContext.
class AdaptiveFrameScheduler {
public:
    using Clock = std::chrono::steady_clock;

    /// Cost measured for the previous LSFG cycle. These are deliberately
    /// vendor-neutral stage measurements; policy compares them to the measured
    /// source/game-work budget rather than to an Xclipse- or A6xx-specific ms cap.
    struct StageCosts {
        double handoffMs{};
        double dispatchMs{};
        double waitIdleMs{};
        double generatedPresentMs{};

        [[nodiscard]] double totalMs() const noexcept {
            return handoffMs + dispatchMs + waitIdleMs + generatedPresentMs;
        }
    };

    struct Diagnostics {
        uint32_t targetFps{};
        double smoothedSourceFps{};
        double requestedGeneratedFrames{};
        double lastStageCostMs{};
        double lastStageCostRatio{};
        std::size_t generationCeiling{};
        std::size_t provenGenerationCeiling{};
        std::size_t lastGeneratedFrameCount{};
        std::size_t cooldownSamples{};
        std::size_t probeSamplesRequired{};
        bool probing{};
        bool lastProbeRejected{};
        const char* lastDecisionReason{"none"};
    };

    AdaptiveFrameScheduler() = default;
    AdaptiveFrameScheduler(uint32_t targetFps, std::size_t maxGeneratedFrames);

    /// Apply a hot-reloaded output target. Changed settings clear fractional,
    /// baseline, probe and cooldown state so the old policy cannot leak through.
    void configure(uint32_t targetFps, std::size_t maxGeneratedFrames);

    /// Observe pure source/game cadence produced after the previous plan plus
    /// the LSFG stage cost incurred by that previous plan, then return the
    /// generated-frame count for the next source interval.
    std::size_t plan(
        std::chrono::nanoseconds sourceInterval,
        const StageCosts& priorStageCosts = {});

    /// Tell the policy when the presentation path could not execute the planned
    /// count (history warmup, timeout/fail-open). This keeps the next sample from
    /// being attributed to work that never actually ran.
    void noteActualGenerationCount(std::size_t count) noexcept {
        lastGeneratedFrameCount_ = count > maxGeneratedFrames_
            ? maxGeneratedFrames_ : count;
    }

    /// Compatibility shim for the existing Android present path. Adaptive FPS
    /// is an objective rather than a source limiter, so this always returns
    /// zero and never intentionally sleeps the game's real-frame presentation.
    std::chrono::nanoseconds delayUntilNextSourceOutput(Clock::time_point now);

    void reset();

    [[nodiscard]] uint32_t targetFps() const { return targetFps_; }
    [[nodiscard]] std::size_t generationCeiling() const { return generationCeiling_; }
    [[nodiscard]] std::size_t provenGenerationCeiling() const {
        return provenGenerationCeiling_;
    }
    [[nodiscard]] Diagnostics diagnostics() const;

private:
    struct LevelStats {
        double sourceFps{};
        double stageCostMs{};
        double stageCostRatio{};
        std::size_t samples{};
    };

    void recordSourceSample(
        std::size_t generationLevel,
        double sourceFps,
        double sourceIntervalMs,
        const StageCosts& stageCosts);
    [[nodiscard]] bool shouldRejectProbe(
        std::size_t probeLevel,
        const char** reason) const;
    [[nodiscard]] std::size_t probeSamplesRequired() const noexcept;
    [[nodiscard]] std::size_t stableSamplesRequired() const noexcept;
    [[nodiscard]] std::size_t rejectedCooldownSamples() const noexcept;
    void logControllerEvent(const char* event, const char* reason = "none") const;

    uint32_t targetFps_{};
    std::size_t maxGeneratedFrames_{};
    double fractionalGeneratedBudget_{};
    double smoothedSourceIntervalSeconds_{};
    double lastSmoothedSourceFps_{};
    double lastRequestedGeneratedFrames_{};
    double lastStageCostMs_{};
    double lastStageCostRatio_{};
    bool hasSmoothedInterval_{false};

    std::size_t generationCeiling_{};
    std::size_t provenGenerationCeiling_{};
    std::size_t lastGeneratedFrameCount_{};
    std::size_t warmupSamplesRemaining_{};
    std::size_t stableDeficitSamples_{};
    std::size_t probeCooldownSamples_{};
    bool lastProbeRejected_{false};
    const char* lastDecisionReason_{"none"};
    std::vector<LevelStats> levelStats_{};
};
