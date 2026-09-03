#include "adaptive_scheduler.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace {
constexpr std::size_t kWarmupSamples = 6;
constexpr double kIntervalSmoothing = 0.25;
constexpr double kStatsSmoothing = 0.25;
constexpr double kTargetSatisfiedRatio = 0.995;
constexpr double kProbeThroughputFloor = 0.96;
constexpr double kProbeSourceFpsFloor = 0.72;
constexpr double kProbeSourceRegressionFloor = 0.92;
constexpr double kProbeStageCostPressure = 0.70;
constexpr double kSustainedStageCostBackoff = 0.90;
constexpr double kAdditionalLevelDemand = 0.05;
constexpr double kProbeWindowSeconds = 0.15;
constexpr double kStableDeficitWindowSeconds = 0.20;
constexpr double kRejectedProbeCooldownSeconds = 1.50;
}

AdaptiveFrameScheduler::AdaptiveFrameScheduler(
        uint32_t targetFps, std::size_t maxGeneratedFrames)
        : targetFps_(targetFps),
          maxGeneratedFrames_(maxGeneratedFrames) {
    reset();
}

void AdaptiveFrameScheduler::configure(
        uint32_t targetFps, std::size_t maxGeneratedFrames) {
    if (targetFps_ == targetFps && maxGeneratedFrames_ == maxGeneratedFrames)
        return;
    targetFps_ = targetFps;
    maxGeneratedFrames_ = maxGeneratedFrames;
    reset();
}

std::size_t AdaptiveFrameScheduler::probeSamplesRequired() const noexcept {
    const double fps = lastSmoothedSourceFps_ > 1.0
        ? lastSmoothedSourceFps_ : 60.0;
    return static_cast<std::size_t>(std::clamp(
        std::lround(fps * kProbeWindowSeconds), 4L, 16L));
}

std::size_t AdaptiveFrameScheduler::stableSamplesRequired() const noexcept {
    const double fps = lastSmoothedSourceFps_ > 1.0
        ? lastSmoothedSourceFps_ : 60.0;
    return static_cast<std::size_t>(std::clamp(
        std::lround(fps * kStableDeficitWindowSeconds), 6L, 24L));
}

std::size_t AdaptiveFrameScheduler::rejectedCooldownSamples() const noexcept {
    const double fps = lastSmoothedSourceFps_ > 1.0
        ? lastSmoothedSourceFps_ : 60.0;
    return static_cast<std::size_t>(std::clamp(
        std::lround(fps * kRejectedProbeCooldownSeconds), 30L, 240L));
}

void AdaptiveFrameScheduler::recordSourceSample(
        std::size_t generationLevel,
        double sourceFps,
        double sourceIntervalMs,
        const StageCosts& stageCosts) {
    if (generationLevel >= levelStats_.size() || !(sourceFps > 0.0)
            || !std::isfinite(sourceFps))
        return;

    const double stageCostMs = stageCosts.totalMs();
    const bool validStageCost = stageCostMs >= 0.0 && std::isfinite(stageCostMs);
    const double stageCostRatio = validStageCost && sourceIntervalMs > 0.0
        ? stageCostMs / sourceIntervalMs : 0.0;

    auto& stats = levelStats_.at(generationLevel);
    if (stats.samples == 0) {
        stats.sourceFps = sourceFps;
        stats.stageCostMs = validStageCost ? stageCostMs : 0.0;
        stats.stageCostRatio = std::isfinite(stageCostRatio) ? stageCostRatio : 0.0;
    } else {
        stats.sourceFps += kStatsSmoothing * (sourceFps - stats.sourceFps);
        if (validStageCost) {
            stats.stageCostMs += kStatsSmoothing * (stageCostMs - stats.stageCostMs);
            if (std::isfinite(stageCostRatio))
                stats.stageCostRatio += kStatsSmoothing
                    * (stageCostRatio - stats.stageCostRatio);
        }
    }
    stats.samples++;
}

bool AdaptiveFrameScheduler::shouldRejectProbe(
        std::size_t probeLevel, const char** reason) const {
    if (reason)
        *reason = "none";
    if (probeLevel == 0 || probeLevel >= levelStats_.size()
            || provenGenerationCeiling_ >= levelStats_.size())
        return false;

    const auto& probe = levelStats_.at(probeLevel);
    const auto& reference = levelStats_.at(provenGenerationCeiling_);
    if (probe.samples < probeSamplesRequired() || reference.samples == 0)
        return false;

    const double probeThroughput = probe.sourceFps
        * static_cast<double>(probeLevel + 1);
    const double referenceThroughput = reference.sourceFps
        * static_cast<double>(provenGenerationCeiling_ + 1);
    const bool throughputRegressed =
        probeThroughput < referenceThroughput * kProbeThroughputFloor;
    const bool sourceRegressed =
        probe.sourceFps < reference.sourceFps * kProbeSourceRegressionFloor;
    const bool sourceFpsCollapsed =
        probe.sourceFps < reference.sourceFps * kProbeSourceFpsFloor;
    const bool stageCostPressure = probe.stageCostRatio > kProbeStageCostPressure;

    if (sourceFpsCollapsed) {
        if (reason) *reason = "source-collapse";
        return true;
    }
    if (stageCostPressure) {
        if (reason) *reason = "stage-cost-pressure";
        return true;
    }
    // A small output-throughput fluctuation on its own is not evidence that a
    // probe is harmful. Require both useful-throughput and source regressions.
    if (throughputRegressed && sourceRegressed) {
        if (reason) *reason = "throughput-and-source-regression";
        return true;
    }
    return false;
}

AdaptiveFrameScheduler::Diagnostics AdaptiveFrameScheduler::diagnostics() const {
    return Diagnostics{
        .targetFps = targetFps_,
        .smoothedSourceFps = lastSmoothedSourceFps_,
        .requestedGeneratedFrames = lastRequestedGeneratedFrames_,
        .lastStageCostMs = lastStageCostMs_,
        .lastStageCostRatio = lastStageCostRatio_,
        .generationCeiling = generationCeiling_,
        .provenGenerationCeiling = provenGenerationCeiling_,
        .lastGeneratedFrameCount = lastGeneratedFrameCount_,
        .cooldownSamples = probeCooldownSamples_,
        .probeSamplesRequired = probeSamplesRequired(),
        .probing = generationCeiling_ > provenGenerationCeiling_,
        .lastProbeRejected = lastProbeRejected_,
        .lastDecisionReason = lastDecisionReason_,
    };
}

void AdaptiveFrameScheduler::logControllerEvent(
        const char* event, const char* reason) const {
    const auto state = diagnostics();
    std::cerr << "lsfg-vk: adaptive"
              << " event=" << event
              << " reason=" << reason
              << " target_fps=" << state.targetFps
              << " source_fps=" << state.smoothedSourceFps
              << " requested_generated=" << state.requestedGeneratedFrames
              << " stage_cost_ms=" << state.lastStageCostMs
              << " stage_cost_ratio=" << state.lastStageCostRatio
              << " ceiling=" << state.generationCeiling
              << " proven_ceiling=" << state.provenGenerationCeiling
              << " last_generated=" << state.lastGeneratedFrameCount
              << " probing=" << (state.probing ? 1 : 0)
              << " probe_samples=" << state.probeSamplesRequired
              << " cooldown_samples=" << state.cooldownSamples
              << '\n';
}

std::size_t AdaptiveFrameScheduler::plan(
        std::chrono::nanoseconds sourceInterval) {
    return plan(sourceInterval, StageCosts{});
}

std::size_t AdaptiveFrameScheduler::plan(
        std::chrono::nanoseconds sourceInterval,
        const StageCosts& priorStageCosts) {
    lastProbeRejected_ = false;
    lastDecisionReason_ = "none";
    if (targetFps_ == 0 || maxGeneratedFrames_ == 0)
        return 0;

    const double intervalSeconds = std::chrono::duration<double>(sourceInterval).count();
    if (!(intervalSeconds > 0.0) || !std::isfinite(intervalSeconds))
        return 0;

    // Pauses, app switches and shader-compilation stalls are not interpolation
    // samples. Re-establish a clean source-only baseline instead of bursting.
    if (intervalSeconds >= 0.250) {
        reset();
        lastDecisionReason_ = "source-stall";
        logControllerEvent("stall-reset", lastDecisionReason_);
        return 0;
    }

    if (!hasSmoothedInterval_) {
        smoothedSourceIntervalSeconds_ = intervalSeconds;
        hasSmoothedInterval_ = true;
    } else {
        smoothedSourceIntervalSeconds_ += kIntervalSmoothing
            * (intervalSeconds - smoothedSourceIntervalSeconds_);
    }

    const double intervalMs = intervalSeconds * 1000.0;
    lastStageCostMs_ = std::max(0.0, priorStageCosts.totalMs());
    lastStageCostRatio_ = intervalMs > 0.0 ? lastStageCostMs_ / intervalMs : 0.0;

    // The interval arriving here is pure game-work cadence produced after the
    // previous LSFG cycle, so attribute source throughput and LSFG cost to the
    // generation level selected by the previous plan.
    recordSourceSample(
        lastGeneratedFrameCount_, 1.0 / intervalSeconds, intervalMs, priorStageCosts);

    lastSmoothedSourceFps_ = 1.0 / smoothedSourceIntervalSeconds_;

    if (warmupSamplesRemaining_ > 0) {
        warmupSamplesRemaining_--;
        fractionalGeneratedBudget_ = 0.0;
        lastRequestedGeneratedFrames_ = 0.0;
        lastGeneratedFrameCount_ = 0;
        if (warmupSamplesRemaining_ == 0) {
            generationCeiling_ = std::min<std::size_t>(1, maxGeneratedFrames_);
            if (generationCeiling_ > 0)
                logControllerEvent("warmup-complete", "baseline-established");
        }
        return 0;
    }

    if (lastSmoothedSourceFps_ >= static_cast<double>(targetFps_) * kTargetSatisfiedRatio) {
        fractionalGeneratedBudget_ = 0.0;
        lastRequestedGeneratedFrames_ = 0.0;
        lastGeneratedFrameCount_ = 0;
        stableDeficitSamples_ = 0;
        lastDecisionReason_ = "target-satisfied";
        return 0;
    }

    const double rawWantedGenerated = std::clamp(
        static_cast<double>(targetFps_) * smoothedSourceIntervalSeconds_ - 1.0,
        0.0,
        static_cast<double>(maxGeneratedFrames_));
    lastRequestedGeneratedFrames_ = rawWantedGenerated;

    // A ceiling above the proven level is a probe. Accept it only when useful
    // throughput/source cadence survive and measured LSFG cost stays well below
    // the source budget. This is intentionally independent of GPU vendor name.
    if (generationCeiling_ > provenGenerationCeiling_
            && generationCeiling_ < levelStats_.size()
            && levelStats_.at(generationCeiling_).samples >= probeSamplesRequired()) {
        const auto probeLevel = generationCeiling_;
        const char* reason = "probe-stable";
        if (shouldRejectProbe(probeLevel, &reason)) {
            levelStats_.at(probeLevel) = {};
            generationCeiling_ = provenGenerationCeiling_;
            probeCooldownSamples_ = rejectedCooldownSamples();
            stableDeficitSamples_ = 0;
            fractionalGeneratedBudget_ = 0.0;
            lastProbeRejected_ = true;
            lastDecisionReason_ = reason;
            logControllerEvent("probe-rejected", reason);
        } else {
            provenGenerationCeiling_ = probeLevel;
            stableDeficitSamples_ = 0;
            lastDecisionReason_ = "probe-stable";
            logControllerEvent("probe-accepted", lastDecisionReason_);
        }
    }

    // A level that was previously sustainable can cease to be so as the device
    // heats or shares GPU time. Back off only under sustained severe measured
    // stage pressure, then allow a later periodic re-probe after cooldown.
    if (generationCeiling_ == provenGenerationCeiling_
            && provenGenerationCeiling_ > 0
            && provenGenerationCeiling_ < levelStats_.size()) {
        const auto& current = levelStats_.at(provenGenerationCeiling_);
        if (current.samples >= probeSamplesRequired() * 2
                && current.stageCostRatio > kSustainedStageCostBackoff) {
            levelStats_.at(provenGenerationCeiling_) = {};
            provenGenerationCeiling_--;
            generationCeiling_ = provenGenerationCeiling_;
            probeCooldownSamples_ = rejectedCooldownSamples();
            stableDeficitSamples_ = 0;
            fractionalGeneratedBudget_ = 0.0;
            lastDecisionReason_ = "sustained-stage-cost-pressure";
            logControllerEvent("proven-level-backoff", lastDecisionReason_);
        }
    }

    if (probeCooldownSamples_ > 0)
        probeCooldownSamples_--;

    // Probe one level at a time only when the target actually requires more
    // interpolation than the proven level can provide. Sample/cooldown windows
    // are time-scaled through measured source FPS, so 30/60/120 Hz sources do
    // not experience radically different controller response times.
    if (generationCeiling_ == provenGenerationCeiling_
            && generationCeiling_ < maxGeneratedFrames_
            && rawWantedGenerated > static_cast<double>(generationCeiling_)
                + kAdditionalLevelDemand) {
        if (probeCooldownSamples_ == 0) {
            stableDeficitSamples_++;
            if (stableDeficitSamples_ >= stableSamplesRequired()) {
                generationCeiling_++;
                stableDeficitSamples_ = 0;
                lastDecisionReason_ = "target-deficit";
                logControllerEvent("probe-start", lastDecisionReason_);
            }
        }
    } else if (rawWantedGenerated <= static_cast<double>(generationCeiling_)
            + kAdditionalLevelDemand) {
        stableDeficitSamples_ = 0;
    }

    if (generationCeiling_ == 0 || rawWantedGenerated <= 0.0) {
        fractionalGeneratedBudget_ = 0.0;
        lastGeneratedFrameCount_ = 0;
        return 0;
    }

    const double wantedGenerated = std::min(
        rawWantedGenerated, static_cast<double>(generationCeiling_));
    fractionalGeneratedBudget_ += wantedGenerated;

    const auto generated = static_cast<std::size_t>(
        std::floor(fractionalGeneratedBudget_ + 1e-6));
    const auto clamped = std::min(generated, generationCeiling_);
    fractionalGeneratedBudget_ -= static_cast<double>(clamped);

    // Preserve fractional carry across ceiling-limited frames. Bound only stale
    // carry large enough to create a future burst after a policy transition.
    fractionalGeneratedBudget_ = std::clamp(
        fractionalGeneratedBudget_, 0.0,
        static_cast<double>(std::max<std::size_t>(1, generationCeiling_)));
    lastGeneratedFrameCount_ = clamped;
    return clamped;
}

std::chrono::nanoseconds AdaptiveFrameScheduler::delayUntilNextSourceOutput(
        Clock::time_point now) {
    (void)now;
    return std::chrono::nanoseconds::zero();
}

void AdaptiveFrameScheduler::reset() {
    fractionalGeneratedBudget_ = 0.0;
    smoothedSourceIntervalSeconds_ = 0.0;
    lastSmoothedSourceFps_ = 0.0;
    lastRequestedGeneratedFrames_ = 0.0;
    lastStageCostMs_ = 0.0;
    lastStageCostRatio_ = 0.0;
    hasSmoothedInterval_ = false;
    generationCeiling_ = 0;
    provenGenerationCeiling_ = 0;
    lastGeneratedFrameCount_ = 0;
    warmupSamplesRemaining_ = targetFps_ > 0 && maxGeneratedFrames_ > 0
        ? kWarmupSamples : 0;
    stableDeficitSamples_ = 0;
    probeCooldownSamples_ = 0;
    lastProbeRejected_ = false;
    lastDecisionReason_ = "none";
    levelStats_.assign(maxGeneratedFrames_ + 1, {});
}
