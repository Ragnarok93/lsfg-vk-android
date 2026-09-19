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
};

enum class AdaptiveFlowDecisionReason : uint8_t {
    None,
    Disabled,
    InvalidTelemetry,
    SchedulerTransition,
    Cooldown,
    InsufficientFlowContribution,
    SustainedPressure,
    SustainedGlobalPressure,
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
    /// Scale-sensitive optical-flow duration, including mipmaps and flow refinement.
    double flowMs{};
    /// Mipmaps portion of flowMs, retained for diagnostics.
    double mipmapsMs{};
    std::size_t generationCount{};
    bool deadlineMissed{false};
    /// Whole-device GPU utilization sampled out-of-band by GameNative.
    double globalGpuUsagePercent{};
    /// True when the global GPU sample is fresh and trustworthy.
    bool globalPressureValid{false};
    /// Whole-output cadence is materially below target or slow-frame pressure is high.
    bool outputDeficit{false};
    /// A synthetic opportunity was rejected/dropped since the previous observation.
    bool syntheticDropPressure{false};
    /// Current timing comes from a cycle that actually generated LSFG output.
    bool generatedWorkSample{true};
    /// True when Adaptive LSFG has just changed/snap/probed/backed-off/reset.
    bool schedulerTransition{false};
    bool valid{false};
};

struct AdaptiveFlowTelemetry {
    float targetScale{1.0F};
    float minimumScale{1.0F};
    float currentScale{1.0F};
    std::size_t stateIndex{};
    bool changed{false};
    AdaptiveFlowDecisionReason reason{AdaptiveFlowDecisionReason::None};
    double pressureRatio{};
    double flowBudgetRatio{};
    double estimatedNextTotalMs{};
    double globalGpuUsagePercent{};
    bool globalPressure{false};
    bool outputDeficit{false};
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
    void selectTargetState();

    bool enabled_{false};
    AdaptiveFlowPreset preset_{AdaptiveFlowPreset::Quality};
    double observedSeconds_{};
    double pressureSeconds_{};
    double headroomSeconds_{};
    double cooldownUntilSeconds_{};
    double schedulerHoldUntilSeconds_{};
    AdaptiveFlowTelemetry telemetry_{};
};
