#include "adaptive_scheduler.hpp"

#include <algorithm>
#include <cmath>

namespace {
constexpr std::size_t kWarmupSamples = 6;
constexpr std::size_t kProbeSamples = 8;
constexpr std::size_t kStableSamplesBeforeProbe = 12;
constexpr std::size_t kRejectedProbeCooldownSamples = 120;
constexpr double kIntervalSmoothing = 0.25;
constexpr double kStatsSmoothing = 0.25;
constexpr double kTargetSatisfiedRatio = 0.995;
constexpr double kProbeThroughputFloor = 0.98;
constexpr double kProbeSourceFpsFloor = 0.70;
constexpr double kAdditionalLevelDemand = 0.05;
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

void AdaptiveFrameScheduler::recordSourceSample(
        std::size_t generationLevel, double sourceFps) {
    if (generationLevel >= levelStats_.size() || !(sourceFps > 0.0)
            || !std::isfinite(sourceFps))
        return;

    auto& stats = levelStats_.at(generationLevel);
    if (stats.samples == 0)
        stats.sourceFps = sourceFps;
    else
        stats.sourceFps += kStatsSmoothing * (sourceFps - stats.sourceFps);
    stats.samples++;
}

bool AdaptiveFrameScheduler::shouldRejectProbe(std::size_t probeLevel) const {
    if (probeLevel == 0 || probeLevel >= levelStats_.size()
            || provenGenerationCeiling_ >= levelStats_.size())
        return false;

    const auto& probe = levelStats_.at(probeLevel);
    const auto& reference = levelStats_.at(provenGenerationCeiling_);
    if (probe.samples < kProbeSamples || reference.samples == 0)
        return false;

    const double probeThroughput = probe.sourceFps
        * static_cast<double>(probeLevel + 1);
    const double referenceThroughput = reference.sourceFps
        * static_cast<double>(provenGenerationCeiling_ + 1);
    const bool throughputRegressed =
        probeThroughput < referenceThroughput * kProbeThroughputFloor;
    const bool sourceFpsCollapsed =
        probe.sourceFps < reference.sourceFps * kProbeSourceFpsFloor;
    return throughputRegressed || sourceFpsCollapsed;
}

std::size_t AdaptiveFrameScheduler::plan(std::chrono::nanoseconds sourceInterval) {
    if (targetFps_ == 0 || maxGeneratedFrames_ == 0)
        return 0;

    const double intervalSeconds = std::chrono::duration<double>(sourceInterval).count();
    if (!(intervalSeconds > 0.0) || !std::isfinite(intervalSeconds))
        return 0;

    // Pauses, app switches and shader-compilation stalls are not interpolation
    // samples. Re-establish a clean source-only baseline instead of bursting.
    if (intervalSeconds >= 0.250) {
        reset();
        return 0;
    }

    if (!hasSmoothedInterval_) {
        smoothedSourceIntervalSeconds_ = intervalSeconds;
        hasSmoothedInterval_ = true;
    } else {
        smoothedSourceIntervalSeconds_ += kIntervalSmoothing
            * (intervalSeconds - smoothedSourceIntervalSeconds_);
    }

    // The interval arriving here was produced after the previous plan, so
    // attribute its source throughput to that previous generation level.
    recordSourceSample(lastGeneratedFrameCount_, 1.0 / intervalSeconds);

    if (warmupSamplesRemaining_ > 0) {
        warmupSamplesRemaining_--;
        fractionalGeneratedBudget_ = 0.0;
        lastGeneratedFrameCount_ = 0;
        if (warmupSamplesRemaining_ == 0)
            generationCeiling_ = std::min<std::size_t>(1, maxGeneratedFrames_);
        return 0;
    }

    const double smoothedSourceFps = 1.0 / smoothedSourceIntervalSeconds_;
    if (smoothedSourceFps >= static_cast<double>(targetFps_) * kTargetSatisfiedRatio) {
        fractionalGeneratedBudget_ = 0.0;
        lastGeneratedFrameCount_ = 0;
        stableDeficitSamples_ = 0;
        return 0;
    }

    const double rawWantedGenerated = std::clamp(
        static_cast<double>(targetFps_) * smoothedSourceIntervalSeconds_ - 1.0,
        0.0,
        static_cast<double>(maxGeneratedFrames_));

    // A ceiling above the proven level is a probe. Accept it only when it
    // improves useful output throughput without collapsing source rendering.
    if (generationCeiling_ > provenGenerationCeiling_
            && generationCeiling_ < levelStats_.size()
            && levelStats_.at(generationCeiling_).samples >= kProbeSamples) {
        const auto probeLevel = generationCeiling_;
        if (shouldRejectProbe(probeLevel)) {
            levelStats_.at(probeLevel) = {};
            generationCeiling_ = provenGenerationCeiling_;
            probeCooldownSamples_ = kRejectedProbeCooldownSamples;
            stableDeficitSamples_ = 0;
            fractionalGeneratedBudget_ = 0.0;
        } else {
            provenGenerationCeiling_ = probeLevel;
            stableDeficitSamples_ = 0;
        }
    }

    if (probeCooldownSamples_ > 0)
        probeCooldownSamples_--;

    // Probe one level at a time only when the target actually requires more
    // interpolation than the proven level can provide.
    if (generationCeiling_ == provenGenerationCeiling_
            && generationCeiling_ < maxGeneratedFrames_
            && rawWantedGenerated > static_cast<double>(generationCeiling_)
                + kAdditionalLevelDemand) {
        if (probeCooldownSamples_ == 0) {
            stableDeficitSamples_++;
            if (stableDeficitSamples_ >= kStableSamplesBeforeProbe) {
                generationCeiling_++;
                stableDeficitSamples_ = 0;
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

    if (clamped == generationCeiling_)
        fractionalGeneratedBudget_ = std::min(fractionalGeneratedBudget_, 0.999999);
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
    hasSmoothedInterval_ = false;
    generationCeiling_ = 0;
    provenGenerationCeiling_ = 0;
    lastGeneratedFrameCount_ = 0;
    warmupSamplesRemaining_ = targetFps_ > 0 && maxGeneratedFrames_ > 0
        ? kWarmupSamples : 0;
    stableDeficitSamples_ = 0;
    probeCooldownSamples_ = 0;
    levelStats_.assign(maxGeneratedFrames_ + 1, {});
}
