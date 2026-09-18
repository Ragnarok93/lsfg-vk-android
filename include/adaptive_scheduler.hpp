#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

struct SourceTimelineCycle {
    bool valid{false};
    uint64_t sourceIndex{};
    uint64_t anchorNs{};
    uint64_t intervalNs{};
    uint64_t sourceDeadlineNs{};

    [[nodiscard]] uint64_t syntheticDeadlineNs(double phase) const;
};

/// Source-arrival-derived presentation clock. It owns no generation policy:
/// each real frame establishes one forward-looking interpolation interval and
/// generated slots can only reference that immutable cycle.
class SourceFrameTimeline {
public:
    SourceTimelineCycle observe(uint64_t sourceArrivalNs,
        std::chrono::nanoseconds observedSourceInterval);
    void reset();

    [[nodiscard]] uint64_t intervalNs() const { return intervalNs_; }

private:
    uint64_t sourceIndex_{0};
    uint64_t intervalNs_{0};
};

struct SyntheticDeadlineSlotDecision {
    double phase{};
    uint64_t deadlineNs{};
    double usableBudgetMs{};
    double predictedCompletionMs{};
    double safetyMarginMs{};
    bool admitted{true};
};

struct SyntheticDeadlineAdmissionPlan {
    bool predictionValid{false};
    std::size_t rejectedCount{};
    std::vector<SyntheticDeadlineSlotDecision> slots;
};

/// Stateless fast-path deadline check. It owns no long-term policy and never
/// changes source cadence, generation density, or Flow Scale.
class SyntheticDeadlineAdmission {
public:
    [[nodiscard]] static SyntheticDeadlineAdmissionPlan evaluate(
        uint64_t nowNs,
        const SourceTimelineCycle& cycle,
        const std::vector<double>& phases,
        double sharedCostMs,
        double perSyntheticCostMs,
        double positivePredictionErrorMarginMs);
};

struct AdaptiveGenerationPlan {
    // Normalized synthetic opportunities inside the next protected source
    // interval. 0.0 is the source-arrival anchor and 1.0 is the source
    // deadline. Creating a slot consumes it; callers must never feed rejected
    // or late slots back into the distributor as catch-up debt.
    std::vector<double> slotPhases;
    double desiredDensity{};
    double governedDensity{};
};

struct AdaptiveSchedulerTelemetry {
    double sourceFps{};
    double smoothedSourceFps{};
    double wantedGeneratedFrames{};
    double governedGeneratedDensity{};
    double fractionalPhase{};
    std::size_t costLimit{};
    std::size_t generatedFrames{};
    bool sourceRateSnapped{false};
    bool costRaised{false};
    bool costBackedOff{false};
    bool costProbe{false};
    bool discontinuityReset{false};
    bool configWarmStart{false};
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

    /// Observe a real/source frame interval and construct deterministic
    /// synthetic opportunities for the following protected source interval.
    /// Slot creation advances fractional phase immediately, so a caller that
    /// rejects a slot cannot create catch-up debt.
    AdaptiveGenerationPlan planSlots(std::chrono::nanoseconds sourceInterval);

    /// Compatibility wrapper for existing count-only callers.
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