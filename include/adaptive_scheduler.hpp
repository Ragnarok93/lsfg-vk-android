#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>

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
    bool configWarmStart{false};
    double fractionalPhase{};
    std::size_t syntheticOpportunitiesCreated{};
};

struct SourceTimelineSample {
    uint64_t sourceIndex{};
    uint64_t intervalNs{};
    uint64_t previousSourceDesiredTimeNs{};
    uint64_t sourceDesiredTimeNs{};
    int64_t sourceDeadlineErrorNs{};
    bool rebased{false};
    bool valid{false};
};

/// Maintains a presentation epoch driven only by real/source arrivals.
///
/// The timeline deliberately has no generated-present API: generated work may
/// query interpolation positions inside the current source interval, but only a
/// subsequent source observation can advance the source deadline.
class SourceProtectedTimeline {
public:
    SourceTimelineSample observe(
        uint64_t sourceArrivalTimeNs,
        std::chrono::nanoseconds sourceInterval,
        bool discontinuity = false);

    [[nodiscard]] uint64_t syntheticDesiredTimeNs(
        const SourceTimelineSample& sample, double interpolationFraction) const;

    void reset();

private:
    bool initialized_{false};
    uint64_t sourceIndex_{0};
    uint64_t sourceDesiredTimeNs_{0};
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
    /// leak into the new schedule. If the scheduler was already observing a
    /// valid runtime cadence, the next valid sample may warm-start the new
    /// generation ceiling instead of re-ramping from one generated frame.
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
    double fractionalOpportunityPhase_{};
    double smoothedSourceIntervalSeconds_{};
    double lastTrustedSourceIntervalSeconds_{};
    bool hasSmoothedInterval_{false};
    // Unlike the current smoothing window, this survives timing discontinuities.
    // It distinguishes an established runtime that resumed before observing a
    // config write from a true first-start/lifecycle reset.
    bool runtimeCadenceEstablished_{false};
    bool reconfigureWarmStartPending_{false};

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

    // Once an established generation level stops preserving useful source
    // cadence, temporarily try one cheaper level. The lower level is retained
    // only when source FPS recovers enough to preserve aggregate throughput.
    double sourcePreservationSinceSeconds_{-1.0};
    double sourcePreservationFpsSum_{};
    std::size_t sourcePreservationSamples_{};
    bool sourcePreservationProbeActive_{false};
    std::size_t sourcePreservationOriginalCost_{};
    double sourcePreservationBaselineFps_{};
    double sourcePreservationProbeStartedSeconds_{};
    double sourcePreservationProbeFpsSum_{};
    std::size_t sourcePreservationProbeSamples_{};

    AdaptiveSchedulerTelemetry telemetry_{};
};