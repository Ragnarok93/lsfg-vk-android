#pragma once

#include <array>
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
    // Compatibility telemetry retained for older log consumers. The unified
    // target-authoritative scheduler deliberately never sets either flag.
    bool costBackedOff{false};
    bool costProbe{false};
    bool discontinuityReset{false};
    bool configWarmStart{false};
    bool capacityPromoted{false};
    bool safeGenerationHintValid{false};
    std::size_t safeGenerationHint{};
    double fractionalPhase{};
    double opportunityIntervalSeconds{};
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
    uint64_t lastIntervalNs_{0};
    uint64_t predictedIntervalNs_{0};
};

enum class SourceCadenceObservation {
    SourceOnly,
    HistoryMaintenance,
    Generated,
};

const char* sourceCadenceObservationName(SourceCadenceObservation observation);

struct SourceProtectionBudgetTelemetry {
    double protectedSourceIntervalMs{};
    double serializedCopyReserveMs{};
    SourceCadenceObservation lastObservation{SourceCadenceObservation::SourceOnly};
    bool baselineValid{false};
    bool copyCostValid{false};
};

/// Protects a source-owned execution budget from self-inflation by generated
/// or history-maintenance work. Only a genuine source-only observation may
/// move the protected baseline slower; LSFG-active cycles may only prove that
/// the source is naturally faster. The reserve is the incremental LSFG source
/// copy cost, never the host-fence wall wait that also contains game rendering.
class SourceProtectionBudgetTracker {
public:
    void observeSource(
        std::chrono::nanoseconds sourceInterval,
        SourceCadenceObservation observation);
    void observeSerializedCopyCost(double copyCostMs);
    [[nodiscard]] double clampTimelineBudget(double timelineBudgetMs) const;
    void reset();

    [[nodiscard]] const SourceProtectionBudgetTelemetry& telemetry() const {
        return telemetry_;
    }

private:
    bool hasBaseline_{false};
    bool hasCopyCostEstimate_{false};
    double baselineIntervalMs_{};
    double serializedCopyReserveMs_{};
    double slowerSourceCandidateMs_{};
    unsigned slowerSourceCandidateSamples_{};
    SourceProtectionBudgetTelemetry telemetry_{};
};

/// Chooses the minimum number of interpolation frames needed to approach an
/// output FPS target. It owns no Vulkan objects, never paces source frames, and
/// is independently testable.
/// Select the fallback compute budget for one admitted frame-generation batch.
/// On source-protected execution the whole real-source interval owns the batch;
/// generic execution keeps its nominal per-slot budget.
double sourceOwnedFramegenBatchBudgetMs(
    double sourceIntervalMs,
    double nominalBatchBudgetMs,
    bool sourceProtectedExecution);

struct DeadlineAdmissionObservation {
    double mipmapsMs{};
    double opticalFlowMs{};
    double totalLsfgMs{};
    std::size_t generationCount{};
    bool valid{false};
};

struct DeadlineAdmissionDecision {
    double predictedMipmapsMs{};
    double predictedOpticalFlowMs{};
    double predictedTotalLsfgMs{};
    double safetyMarginMs{};
    double usableBudgetMs{};
    double deliveryReserveMs{};
    double effectiveUsableBudgetMs{};
    bool wouldAdmit{false};
    bool valid{false};
};

/// Predicts whether one synthetic batch should fit inside the source-owned
/// presentation budget. This class is deliberately policy-local: it estimates
/// cost and makes a fast admission comparison, but it never changes source
/// timing, fractional demand, Flow Scale, or the long-term generation ceiling.
class DeadlineAdmissionPredictor {
public:
    void observe(const DeadlineAdmissionObservation& observation);
    /// Learn the source-thread blocking cost at the protected private-device
    /// completion boundary. On host-bounded Adreno this includes queue residency
    /// that GPU shader timestamps do not see, so it is part of the source-owned
    /// admission cost rather than a presentation-only reserve.
    void observeBlockingCompletion(
        std::size_t generationCount, double completionMs);
    /// During a protected source-only recovery cycle, relax only queue-residency
    /// overhead toward the last measured GPU batch cost. Never decays below
    /// measured GPU work and therefore cannot invent synthetic capacity.
    void observeSourceOnlyRecovery();
    /// Learn unmodeled submit-to-delivery pressure only from a frame that was
    /// admitted but still missed its synthetic deadline/WSI opportunity.
    void observeDeliveryMiss(double latenessMs);
    /// Slowly relax the learned delivery reserve after a fully queued batch.
    void observeDeliverySuccess();
    [[nodiscard]] DeadlineAdmissionDecision predict(
        std::size_t generationCount, double usableBudgetMs) const;
    /// Return the largest generated-frame count whose evenly-spaced prefix
    /// deadlines fit inside one predicted source interval. This is a capacity
    /// hint only; per-cycle admission remains authoritative.
    [[nodiscard]] std::size_t safeGenerationHint(
        std::size_t maxGenerationCount, double sourceIntervalMs) const;
    /// Deferred/source-protected execution may complete generated work any time
    /// before the next real-source boundary. Unlike safeGenerationHint(), this
    /// does not reinterpret ideal interpolation slots as compute deadlines.
    [[nodiscard]] std::size_t safeBatchGenerationHint(
        std::size_t maxGenerationCount, double sourceProtectionBudgetMs) const;
    [[nodiscard]] bool hasEstimate() const { return hasEstimate_; }
    void reset();

private:
    struct BatchCostEstimate {
        bool valid{false};
        bool blockingCompletionObserved{false};
        double mipmapsMs{};
        double opticalFlowMs{};
        double gpuTotalLsfgMs{};
        double totalLsfgMs{};
    };

    static constexpr std::size_t kTrackedBatchCounts = 4;
    static constexpr double kRecoveryEwmaAlpha = 0.12;
    static constexpr double kUnknownBatchSafetyRatio = 1.10;
    static constexpr double kSafetyMarginRatio = 0.12;
    static constexpr double kSafetyMarginFloorMs = 0.35;
    static constexpr double kDeliveryReserveFloorMs = 0.50;
    static constexpr double kDeliveryReserveMaxMs = 8.0;
    static constexpr double kDeliveryReserveAlpha = 0.25;
    static constexpr double kDeliveryReserveSuccessDecay = 0.95;

    bool hasEstimate_{false};
    std::array<BatchCostEstimate, kTrackedBatchCounts> batchEstimates_{};
    double deliveryReserveMs_{0.0};
};

struct FixedSourceCadenceTelemetry {
    double baselineSourceFps{};
    double intervalRatio{1.0};
    std::size_t requestedGeneratedFrames{};
    std::size_t generationLimit{};
    bool backedOff{false};
    bool raised{false};
    bool baselineValid{false};
};

/// Protects real/source cadence in Fixed frame-generation mode.
///
/// This governor never paces source frames and never changes interpolation
/// positions. The user-selected multiplier is a ceiling: synthetic cost begins
/// conservatively, rises one level at a time after stable cadence, and backs
/// off when the preceding generated load materially stretches source intervals
/// relative to a baseline learned without generated-frame work.
class FixedSourceCadenceGovernor {
public:
    std::size_t plan(
        std::chrono::nanoseconds sourceInterval,
        std::size_t requestedGeneratedFrames,
        std::size_t previousDispatchedGeneratedFrames,
        bool generationAllowed,
        SourceCadenceObservation previousObservation =
            SourceCadenceObservation::HistoryMaintenance);

    void reset();

    [[nodiscard]] const FixedSourceCadenceTelemetry& telemetry() const {
        return telemetry_;
    }

private:
    bool hasBaseline_{false};
    double baselineIntervalSeconds_{};
    std::size_t generationLimit_{1};
    double pressureSeconds_{};
    double recoverySeconds_{};
    double cooldownSeconds_{};
    FixedSourceCadenceTelemetry telemetry_{};
};


enum class GeneratedPresentationCapChangeReason {
    None,
    RejectionProbe,
    ProfitabilityKeepLower,
    ProfitabilityRestoreHigher,
    RecoveryEvidenceRaise,
    TargetDeficitProbeSuccess,
    SubOneDutyLower,
    SubOneDutyRecover,
};

const char* generatedPresentationCapChangeReasonName(
    GeneratedPresentationCapChangeReason reason);

struct GeneratedPresentationCapacityContext {
    bool outputDeficit{false};
    bool deadlineCapacityValid{false};
    std::size_t safeGenerationHint{};
    std::size_t schedulerCostLimit{};
    bool sourceInsideBudget{false};
    int64_t sourceDeadlineErrorNs{};
    bool higherCapacityProven{false};
};

struct GeneratedPresentationCapacityTelemetry {
    std::size_t generationCap{};
    double singleFrameDuty{1.0};
    double wsiRejectionRatio{};
    unsigned rejectionEvidence{};
    double recoveryEvidence{};
    uint64_t attemptedGeneratedFrames{};
    uint64_t acceptedGeneratedFrames{};
    double deliveredEfficiency{};
    double acceptedFramesEwma{};
    std::size_t highestUsefulCapacity{};
    GeneratedPresentationCapChangeReason lastChangeReason{
        GeneratedPresentationCapChangeReason::None};
    bool lastChangeOutputDeficit{false};
    bool upwardProbePending{false};
    bool provisionalLowerActive{false};
    bool pressure{false};
    bool lowered{false};
    bool raised{false};
};

/// Learns downstream swapchain capacity independently from GPU generation
/// capacity. It never blocks on WSI and never changes source pacing.
class GeneratedPresentationCapacityTracker {
public:
    void configure(std::size_t maxGeneratedFrames);
    [[nodiscard]] std::size_t limit(std::size_t requested);
    [[nodiscard]] std::size_t limit(
        std::size_t requested,
        const GeneratedPresentationCapacityContext& context);
    void observe(std::size_t attempted, std::size_t wsiRejected);
    void observe(
        std::size_t attempted,
        std::size_t accepted,
        std::size_t wsiRejected,
        const GeneratedPresentationCapacityContext& context);
    void reset();

    [[nodiscard]] const GeneratedPresentationCapacityTelemetry& telemetry() const {
        return telemetry_;
    }

private:
    std::size_t maxGeneratedFrames_{};
    unsigned rejectionEvidence_{};
    double recoveryEvidence_{};
    double singleFramePhase_{};
    bool hasObservation_{false};
    double acceptedFramesEwma_{};
    double efficiencyEwma_{};

    bool provisionalLowerActive_{false};
    std::size_t provisionalPreviousCap_{};
    unsigned provisionalSamples_{};
    double provisionalAcceptedSum_{};
    double provisionalEfficiencySum_{};
    double provisionalBaselineAccepted_{};
    double provisionalBaselineEfficiency_{};

    bool upwardProbePending_{false};
    bool upwardProbeInFlight_{false};
    std::size_t upwardProbeAttempted_{};

    GeneratedPresentationCapacityTelemetry telemetry_{};
};

struct LsfgOutputCadenceSnapshot {
    bool valid{false};
    bool targeted{false};
    double outputFps{};
    double coverageSeconds{};
    bool deficitConfirmed{false};
    bool targetSatisfiedConfirmed{false};
};

/// Allocation-free rolling output estimator for the LSFG source+generated
/// presentation domain. The one-second RuntimeMetrics window remains logging
/// only; control decisions use this shorter, fresher window.
class LsfgOutputCadenceTracker {
public:
    void configure(bool targeted, uint32_t targetFps);
    void observe(
        std::chrono::nanoseconds elapsed,
        std::size_t sourceFrames,
        std::size_t generatedFrames);
    void reset();

    [[nodiscard]] const LsfgOutputCadenceSnapshot& snapshot() const {
        return snapshot_;
    }

private:
    struct Sample {
        double seconds{};
        std::size_t frames{};
    };

    void clearWindow();
    void rebuildSnapshot(double evidenceSeconds);

    static constexpr std::size_t kSampleCapacity = 128;
    std::array<Sample, kSampleCapacity> samples_{};
    std::size_t sampleCount_{};
    std::size_t nextSample_{};
    bool targeted_{false};
    uint32_t targetFps_{};
    double deficitSeconds_{};
    double satisfiedSeconds_{};
    LsfgOutputCadenceSnapshot snapshot_{};
};

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

    /// Supply a predictor-derived capacity hint for the next generation level.
    /// Invalid hints disable the early-promotion path without affecting the
    /// ordinary sustained-demand governor.
    void setSafeGenerationHint(std::size_t hint, bool valid);

    /// Make target demand authoritative. Resource/cadence estimates remain
    /// telemetry only and may not lower the synthetic generation ceiling.
    void setGenerationFirst(bool enabled);

    /// Supply the last clean, LSFG-independent source interval. When valid,
    /// source protection has priority over target seeking: synthetic cost may
    /// not rise while the observed source cadence is materially degraded.
    void setSourceProtectionBaseline(double intervalMs, bool valid);

    void reset();

    [[nodiscard]] uint32_t targetFps() const { return targetFps_; }
    [[nodiscard]] std::size_t maxGeneratedFrames() const { return maxGeneratedFrames_; }
    [[nodiscard]] const AdaptiveSchedulerTelemetry& telemetry() const { return telemetry_; }

private:
    void resetRuntimeState();
    void resetSourceCadenceWindow();
    void resetUnmetDemand();
    void updateSourceRate(double intervalSeconds);
    [[nodiscard]] double robustSourceIntervalSeconds() const;
    void updateCostLimit(double wantedGeneratedFrames, double intervalSeconds);

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

    // Robust cadence estimation uses only recent trusted real-source intervals.
    // Individual present bursts/hitches cannot hard-snap the scheduler state.
    static constexpr std::size_t kSourceCadenceWindow = 9;
    std::array<double, kSourceCadenceWindow> recentSourceIntervals_{};
    std::size_t recentSourceIntervalCount_{};
    std::size_t recentSourceIntervalCursor_{};

    std::size_t safeGenerationHint_{};
    bool safeGenerationHintValid_{false};
    bool generationFirst_{false};
    unsigned capacityRaiseSamples_{};
    unsigned stableCadenceSamples_{};

    double observedTimeSeconds_{};
    std::size_t costLimit_{};

    bool sourceProtectionBaselineValid_{false};
    double sourceProtectionBaselineSeconds_{};
    unsigned sourceDegradationSamples_{};
    double sourceProtectionHoldUntilSeconds_{};

    // Target demand may raise the ceiling only while source protection is
    // healthy. A clean baseline is authoritative over self-inflicted slowdown.
    double unmetDemandSinceSeconds_{-1.0};

    AdaptiveSchedulerTelemetry telemetry_{};
};
