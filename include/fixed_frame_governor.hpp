#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

struct FixedFrameGovernorTelemetry {
    double sourceFps{};
    double smoothedSourceFps{};
    std::size_t requestedGeneratedFrames{};
    std::size_t costLimit{};
    std::size_t generatedFrames{};
    bool sourceRateSnapped{false};
    bool costRaised{false};
    bool costBackedOff{false};
    bool costProbe{false};
    bool refreshLimited{false};
    bool discontinuityReset{false};
};

/// Internal governor for fixed Off/2x/3x/4x modes. It can reduce the amount of
/// interpolation work beneath the requested fixed multiplier when that work is
/// not sustainable, but it never owns source pacing and never turns an enabled
/// fixed mode into a zero-generation/source-only cycle.
class FixedFrameGovernor {
public:
    void configure(bool enabled, std::size_t requestedMultiplier,
        uint32_t displayRefreshHz = 0);
    std::size_t plan(std::chrono::nanoseconds sourceInterval);
    void reset();

    [[nodiscard]] bool enabled() const { return enabled_; }
    [[nodiscard]] std::size_t requestedGeneratedFrames() const {
        return requestedGeneratedFrames_;
    }
    [[nodiscard]] uint32_t displayRefreshHz() const { return displayRefreshHz_; }
    [[nodiscard]] const FixedFrameGovernorTelemetry& telemetry() const {
        return telemetry_;
    }

private:
    void resetMeasurements(bool resetCostLimit);
    void resetRateCandidates();
    bool updateSourceRate(double intervalSeconds);
    [[nodiscard]] std::size_t refreshGenerationCeiling(double sourceFps) const;
    [[nodiscard]] std::size_t safeOutput() const;

    bool enabled_{false};
    std::size_t requestedGeneratedFrames_{};
    uint32_t displayRefreshHz_{};

    double smoothedSourceIntervalSeconds_{};
    bool hasSmoothedInterval_{false};
    unsigned slowRateSamples_{};
    unsigned fastRateSamples_{};
    double slowIntervalSum_{};
    double fastIntervalSum_{};

    double observedSeconds_{};
    double stableSinceSeconds_{};
    std::size_t costLimit_{};
    bool pendingRaise_{false};
    bool probeAfterBackoff_{false};
    bool pendingRaiseWasProbe_{false};
    double pendingRaiseBaselineFps_{};
    double pendingRaiseSeconds_{};
    double holdUntilSeconds_{};

    FixedFrameGovernorTelemetry telemetry_{};
};
