#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

/// User-facing quality envelopes for Adaptive Flow Scale.
enum class AdaptiveFlowPreset : uint8_t {
    Quality,
    Balanced,
    Low,
    Auto,
};

enum class AdaptiveFlowDecisionReason : uint8_t {
    None,
    Disabled,
    InvalidTelemetry,
    FlowTransition,
    FlowTransitionSettle,
    SchedulerTransition,
    Cooldown,
    InsufficientFlowContribution,
    SustainedPressure,
    SustainedGlobalPressure,
    SustainedOutputPressure,
    SustainedSourcePressure,
    ExploratorySourcePressure,
    EvaluatingDownstep,
    DownstepBenefitConfirmed,
    DownstepReverted,
    InsufficientRecoveryHeadroom,
    SustainedHeadroom,
};

struct AdaptiveFlowObservation {
    /// Real source-cycle duration used only to advance controller time.
    std::chrono::nanoseconds elapsed{};
    /// Budget available to the LSFG work for this operating point.
    double frameBudgetMs{};
    /// Completed previous-cycle LSFG GPU duration.
    double totalLsfgMs{};
    /// Scale-sensitive optical-flow/refinement duration excluding mipmaps.
    double flowMs{};
    /// Mipmaps duration retained separately for diagnostics and total-cost attribution.
    double mipmapsMs{};
    std::size_t generationCount{};
    bool deadlineMissed{false};
    /// Explicit compute/deadline pressure, excluding downstream WSI losses.
    bool computeDeadlinePressure{false};
    /// Downstream synthetic presentation pressure. This is never treated as
    /// compute pressure unless global GPU pressure also makes Flow a plausible
    /// actuator.
    bool wsiPresentationPressure{false};
    double wsiLossRate{};
    double sourceFps{};
    /// Clean source cadence target used by Fixed multiplier mode.
    double sourceTargetFps{};
    /// True whenever Fixed multiplier mode is active, even before a clean
    /// source reference has been established.
    bool fixedMultiplierMode{false};
    double outputFps{};
    /// Required output cadence: Adaptive LSFG target or fixed multiplier base.
    double outputTargetFps{};
    bool outputCadenceValid{false};
    bool outputTargeted{false};
    bool outputTargetSatisfied{false};
    /// True when outputTargetFps is source cadence times the fixed multiplier.
    bool fixedMultiplierBaseTarget{false};
    /// Whole-device GPU utilization sampled out-of-band by GameNative.
    double globalGpuUsagePercent{};
    /// True when the global GPU sample is fresh and trustworthy.
    bool globalPressureValid{false};
    /// Android thermal status (0..6) sampled out-of-band by GameNative.
    /// This is advisory and can only accelerate an already-established Flow pressure signal.
    int thermalStatus{0};
    bool thermalPressureValid{false};
    /// Whole-output cadence is materially below target or slow-frame pressure is high.
    bool outputDeficit{false};
    /// A synthetic opportunity was rejected/dropped since the previous observation.
    bool syntheticDropPressure{false};
    /// Current timing comes from a cycle that actually generated LSFG output.
    bool generatedWorkSample{true};
    /// Timing is retained from the most recent real generated batch. This is
    /// explicit provenance, not permission to treat arbitrary history-only
    /// telemetry as fresh GPU work.
    bool retainedGeneratedTimingSample{false};
    /// True when Adaptive LSFG has just changed/snap/probed/backed-off/reset.
    bool schedulerTransition{false};
    /// True while a pending Flow graph is rebuilding temporal history.
    bool flowTransition{false};
    bool valid{false};
};

struct AdaptiveFlowTelemetry {
    float targetScale{1.0F};
    float minimumScale{1.0F};
    float currentScale{1.0F};
    std::size_t stateIndex{};
    std::size_t stateCount{};
    bool changed{false};
    AdaptiveFlowDecisionReason reason{AdaptiveFlowDecisionReason::None};
    double pressureRatio{};
    double flowBudgetRatio{};
    double estimatedNextTotalMs{};
    double globalGpuUsagePercent{};
    bool globalPressure{false};
    int thermalStatus{0};
    bool thermalPressure{false};
    bool computePressure{false};
    bool wsiPressure{false};
    bool downstepEvaluationActive{false};
    bool outputDeficit{false};
    bool outputPressure{false};
    bool sourcePressure{false};
    bool exploratorySourcePressure{false};
    double sourceReferenceFps{};
};

/// A quality-seeking governor for Flow Scale. It owns no Vulkan objects and
/// consumes only already-completed timing samples, so it cannot synchronize the
/// CPU with the GPU. The Vulkan layer owns applying the selected discrete state.
class AdaptiveFlowController {
public:
    AdaptiveFlowController() = default;
    explicit AdaptiveFlowController(AdaptiveFlowPreset preset);

    void configure(bool enabled, AdaptiveFlowPreset preset);
    float observe(const AdaptiveFlowObservation& observation);
    void reset();

    [[nodiscard]] bool enabled() const { return enabled_; }
    [[nodiscard]] AdaptiveFlowPreset preset() const { return preset_; }
    [[nodiscard]] float currentScale() const { return telemetry_.currentScale; }
    [[nodiscard]] const AdaptiveFlowTelemetry& telemetry() const { return telemetry_; }

    static std::span<const float> statesForPreset(AdaptiveFlowPreset preset);
    static const char* presetName(AdaptiveFlowPreset preset);
    static const char* reasonName(AdaptiveFlowDecisionReason reason);

private:
    void resetEvidence();
    void resetFixedExploration();
    void selectTargetState();

    bool enabled_{false};
    AdaptiveFlowPreset preset_{AdaptiveFlowPreset::Quality};
    double observedSeconds_{};
    double pressureSeconds_{};
    double headroomSeconds_{};
    double cooldownUntilSeconds_{};
    double schedulerHoldUntilSeconds_{};
    bool flowTransitionHoldActive_{false};
    double flowTransitionSettleUntilSeconds_{};

    bool downstepEvaluationActive_{false};
    bool downstepBenefitSeen_{false};
    // Direct missed-output pressure uses a shorter control loop, but retains
    // the same post-change benefit check before another state is selected.
    bool downstepOutputDriven_{false};
    bool downstepSourceDriven_{false};
    bool downstepExploratorySourceDriven_{false};
    std::size_t downstepPreviousIndex_{};
    double downstepEvaluationStartedSeconds_{};
    double downstepBaselinePressureRatio_{};
    double downstepBaselineFlowMs_{};
    double downstepBaselineTotalMs_{};
    double downstepBaselineSourceFps_{};
    double downstepBaselineOutputFps_{};
    double downstepBaselineWsiLossRate_{};
    double downstepBaselineGlobalGpuPercent_{};
    bool downstepBaselineOutputValid_{false};
    bool downstepBaselineComputePressure_{false};
    bool downstepBaselineWsiPressure_{false};
    bool downstepBaselineGlobalPressure_{false};

    // When Fixed mode starts without a trustworthy source-only baseline,
    // lower Flow experimentally and retain only steps that measurably recover
    // source cadence. This reference never becomes the clean fixed target.
    bool fixedExplorationReferenceValid_{false};
    bool fixedExplorationProbePending_{false};
    double fixedExplorationBestSourceFps_{};

    AdaptiveFlowTelemetry telemetry_{};
};
