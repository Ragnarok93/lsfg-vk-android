#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>

enum class AdaptiveCostBackoffReason {
    None,
    RaiseCausalSourceDrop,
    SourcePreservationProbe,
};

enum class AdaptiveWarmStartReason {
    None,
    Config,
    Discontinuity,
};

const char* adaptiveCostBackoffReasonName(AdaptiveCostBackoffReason reason);
const char* adaptiveWarmStartReasonName(AdaptiveWarmStartReason reason);

struct AdaptiveSchedulerTelemetry {
    double sourceFps{};
    double robustSourceFps{};
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
    bool capacityPromoted{false};
    AdaptiveCostBackoffReason costBackoffReason{
        AdaptiveCostBackoffReason::None};
    AdaptiveWarmStartReason warmStartReason{
        AdaptiveWarmStartReason::None};
    bool safeGenerationHintValid{false};
    std::size_t safeGenerationHint{};
    std::size_t provenCostLimit{};
    double raiseBaselineSourceFps{};
    double raiseDropEvidenceSeconds{};
    double stableCadenceSeconds{};
    double recoveryBaselineSourceFps{};
    double recoveryThresholdSourceFps{};
    double establishedSourceFps{};
    // Diagnostic-only source cadence fields retained for runtime telemetry.
    // They must not gate presentation-cap or target-deficit decisions.
    double sourceCadenceRatio{1.0};
    double sourceBudgetMinRatio{0.0};
    bool sourcePreservationActive{false};
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

/// Resolve the source-owned admission deadline for the current source cycle.
/// A normal valid deadline is returned unchanged. If an ordinary timing rebase
/// left that deadline behind the admission point, establish one fresh future
/// source interval instead of permanently classifying every synthetic
/// opportunity as already late.
[[nodiscard]] uint64_t sourceOwnedAdmissionDeadlineNs(
    const SourceTimelineSample& sample, uint64_t admissionNowNs);

/// Cold-start admission policy for Adaptive FG. An uninitialized GPU-cost
/// predictor may conservatively admit one generated frame when enough source
/// slack exists. After a bounded number of starved opportunities, a smaller
/// positive-slack probe is allowed so the predictor cannot deadlock waiting for
/// a generated-work sample that admission itself prevents.
[[nodiscard]] std::size_t adaptiveAdmissionBootstrapGeneratedCount(
    std::size_t plannedGeneratedFrames,
    bool predictorInitialized,
    double remainingSourceSlackMs,
    double sourceIntervalMs,
    unsigned starvedOpportunities);

/// Opportunistically request one replacement synthetic opportunity when actual
/// delivered output is below target. This never changes scheduler fractional
/// debt: it is bounded by the configured maximum, the proven scheduler ceiling,
/// the deadline predictor capacity, and the accepted source-cadence budget.
[[nodiscard]] std::size_t adaptiveDeficitCompensatedGeneratedCount(
    std::size_t plannedGeneratedFrames,
    std::size_t maxGeneratedFrames,
    bool outputDeficit,
    bool deadlineCapacityValid,
    std::size_t safeGenerationHint,
    std::size_t provenCostLimit,
    double sourceCadenceRatio,
    double sourceBudgetMinRatio);

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

/// Chooses the minimum number of interpolation frames needed to approach an
/// output FPS target. It owns no Vulkan objects, never paces source frames, and
/// is independently testable.
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
    [[nodiscard]] bool hasEstimate() const { return hasEstimate_; }
    void reset();

private:
    static constexpr double kEwmaAlpha = 0.20;
    static constexpr double kSafetyMarginRatio = 0.12;
    static constexpr double kSafetyMarginFloorMs = 0.35;
    static constexpr double kDeliveryReserveFloorMs = 0.50;
    static constexpr double kDeliveryReserveMaxMs = 8.0;
    static constexpr double kDeliveryReserveAlpha = 0.25;
    static constexpr double kDeliveryReserveSuccessDecay = 0.95;

    bool hasEstimate_{false};
    double mipmapsMs_{0.0};
    double opticalFlowMs_{0.0};
    double perGeneratedMs_{0.0};
    double deliveryReserveMs_{0.0};
};

enum class GeneratedPresentationCapChangeReason {
    None,
    RejectionProbe,
    ProfitabilityKeepLower,
    ProfitabilityRestoreHigher,
    RecoveryEvidenceRaise,
    TargetDeficitProbeSuccess,
    TargetDeficitProbeRejected,
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
    std::size_t provenCostLimit{};
    double sourceCadenceRatio{1.0};
    double sourceBudgetMinRatio{1.0};
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
    GeneratedPresentationCapChangeReason lastChangeReason{
        GeneratedPresentationCapChangeReason::None};
    bool lastChangeOutputDeficit{false};
    bool upwardProbePending{false};
    bool pressure{false};
    bool lowered{false};
    bool raised{false};
};

/// Learns downstream swapchain capacity independently from GPU generation
/// capacity. A lower cap is a provisional throughput experiment rather than a
/// permanent reaction to raw rejection count. Target deficit and proven compute
/// capacity can trigger bounded upward presentation probes. This class never
/// changes scheduler/proven-cost state or source pacing.
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
    double provisionalBaselineSourceRatio_{1.0};

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

    void reset();

    [[nodiscard]] uint32_t targetFps() const { return targetFps_; }
    [[nodiscard]] std::size_t maxGeneratedFrames() const { return maxGeneratedFrames_; }
    [[nodiscard]] const AdaptiveSchedulerTelemetry& telemetry() const { return telemetry_; }

private:
    void resetRuntimeState(bool preserveLearnedState = false);
    void resetSourceCadenceWindow();
    void resetUnmetDemand();
    void updateSourceRate(double intervalSeconds);
    [[nodiscard]] double robustSourceIntervalSeconds() const;
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
    bool discontinuityWarmStartPending_{false};

    double establishedSourceFps_{};
    double establishedSourceCoverageSeconds_{};
    std::size_t provenCostLimit_{};

    // Robust cadence estimation uses only recent trusted real-source intervals.
    // Individual present bursts/hitches cannot hard-snap the scheduler state.
    static constexpr std::size_t kSourceCadenceWindow = 9;
    std::array<double, kSourceCadenceWindow> recentSourceIntervals_{};
    std::size_t recentSourceIntervalCount_{};
    std::size_t recentSourceIntervalCursor_{};

    std::size_t safeGenerationHint_{};
    bool safeGenerationHintValid_{false};
    unsigned capacityRaiseSamples_{};
    unsigned stableCadenceSamples_{};
    double stableCadenceSeconds_{};
    double pendingRaiseSourceDropEvidenceSeconds_{};
    double pendingRaiseLastEvaluationTimeSeconds_{};

    double observedTimeSeconds_{};
    std::size_t costLimit_{};
    bool pendingCostRaise_{false};
    bool probeAfterBackoff_{false};
    bool pendingRaiseWasProbe_{false};
    double pendingRaiseBaselineFps_{};
    double pendingRaiseTimeSeconds_{};
    double lastCostChangeTimeSeconds_{-1.0};
    double lastBackoffTimeSeconds_{-1.0};
    AdaptiveCostBackoffReason lastBackoffReason_{
        AdaptiveCostBackoffReason::None};
    double successfulProbeHoldUntilSeconds_{};
    double raiseHoldUntilSeconds_{};

    // Raising the generation ceiling requires a sustained output deficit. The
    // source-rate average collected during that observation period becomes the
    // pre-raise baseline used to decide whether the additional LSFG work caused
    // a subsequent source-FPS regression.
    double unmetDemandSinceSeconds_{-1.0};
    double unmetSourceFpsSum_{};
    std::size_t unmetSourceFpsSamples_{};

    double backoffRecoverySinceSeconds_{-1.0};
    double backoffRecoveryFpsSum_{};
    std::size_t backoffRecoverySamples_{};
    double backoffRecoveryBaselineFps_{};
    bool backoffRecoveryReady_{false};

    // Legacy source-preservation state is retained for diagnostic/state-layout
    // continuity only. Source cadence no longer demotes generation density.
    double sourcePreservationSinceSeconds_{-1.0};
    double sourcePreservationFpsSum_{};
    std::size_t sourcePreservationSamples_{};
    bool sourcePreservationProbeActive_{false};
    std::size_t sourcePreservationOriginalCost_{};
    double sourcePreservationBaselineFps_{};
    double sourcePreservationProbeStartedSeconds_{};
    double sourcePreservationProbeFpsSum_{};
    std::size_t sourcePreservationProbeSamples_{};
    double sourcePreservationProbeHoldUntilSeconds_{};

    AdaptiveSchedulerTelemetry telemetry_{};
};