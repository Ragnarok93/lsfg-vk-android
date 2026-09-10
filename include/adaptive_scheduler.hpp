#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

struct AdaptiveSchedulerTelemetry {
    double sourceFps{};
    double smoothedSourceFps{};
    double wantedGeneratedFrames{};
    std::size_t costLimit{};
    std::size_t generatedFrames{};
    bool sourceRateSnapped{false};
    bool costRaised{false};
    bool costBackedOff{false};
    bool costProbe{false};
    bool discontinuityReset{false};
};

/// Chooses the minimum number of interpolation frames needed to approach an
/// output FPS target. It owns no Vulkan objects, never paces source frames, and
/// is independently testable.
class AdaptiveFrameScheduler {
public:
    AdaptiveFrameScheduler() = default;
    AdaptiveFrameScheduler(uint32_t targetFps, std::size_t maxGeneratedFrames);

    /// Apply a hot-reloaded target. A changed target clears fractional,
    /// measurement, and generation-cost state so the previous target cannot
    /// leak into the new schedule.
    void configure(uint32_t targetFps, std::size_t maxGeneratedFrames);

    /// Observe a real/source frame interval and return the number of generated
    /// frames for this source cycle. This is an output planner only: it never
    /// sleeps and never modifies source pacing.
    std::size_t plan(std::chrono::nanoseconds sourceInterval);

    void reset();

    [[nodiscard]] uint32_t targetFps() const { return targetFps_; }
    [[nodiscard]] std::size_t maxGeneratedFrames() const { return maxGeneratedFrames_; }
    [[nodiscard]] const AdaptiveSchedulerTelemetry& telemetry() const { return telemetry_; }

private:
    void resetRuntimeState();
    void resetRateChangeCandidates();
    void resetUnmetDemand();
    void updateSourceRate(double intervalSeconds);
    void updateCostLimit(double wantedGeneratedFrames);

    uint32_t targetFps_{};
    std::size_t maxGeneratedFrames_{};
    double fractionalGeneratedBudget_{};
    double smoothedSourceIntervalSeconds_{};
    bool hasSmoothedInterval_{false};

    // Source-rate decreases need to be recognized quickly so a heavier scene
    // can receive more generation. Apparent source-rate increases are held to
    // a stricter confirmation threshold because Android/WSI present bursts can
    // contain several very short intervals without representing sustainable
    // game throughput.
    unsigned slowRateChangeSamples_{};
    unsigned fastRateChangeSamples_{};
    double slowIntervalAccumulatorSeconds_{};
    double fastIntervalAccumulatorSeconds_{};

    double observedTimeSeconds_{};
    std::size_t costLimit_{};
    bool pendingCostRaise_{false};
    bool probeAfterBackoff_{false};
    bool pendingRaiseWasProbe_{false};
    double pendingRaiseBaselineFps_{};
    double pendingRaiseTimeSeconds_{};
    double lastCostChangeTimeSeconds_{-1.0};
    double lastBackoffTimeSeconds_{-1.0};
    double successfulProbeHoldUntilSeconds_{};
    double raiseHoldUntilSeconds_{};

    // Raising the generation ceiling requires a sustained output deficit. The
    // source-rate average collected during that observation period becomes the
    // pre-raise baseline used to decide whether the additional LSFG work caused
    // a subsequent source-FPS regression.
    double unmetDemandSinceSeconds_{-1.0};
    double unmetSourceFpsSum_{};
    std::size_t unmetSourceFpsSamples_{};

    AdaptiveSchedulerTelemetry telemetry_{};
};
