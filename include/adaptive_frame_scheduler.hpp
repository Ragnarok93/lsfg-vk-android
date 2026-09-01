#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * Converts a final-output FPS target into a bounded number of generated frames
 * for each arriving source frame. Fractional ratios are carried across frames,
 * so 30 -> 45 FPS alternates between zero and one generated frame instead of
 * rounding the whole session up to 60 FPS.
 */
class AdaptiveFrameScheduler {
public:
    AdaptiveFrameScheduler(bool enabled, uint32_t targetFps, size_t maxGenerated)
        : enabled(enabled), targetFps(targetFps), maxGenerated(maxGenerated) {}

    size_t generatedFrames(uint64_t nowNs) {
        if (!enabled) return maxGenerated;
        if (targetFps == 0 || maxGenerated == 0) return 0;
        if (!hasPreviousSource) {
            previousSourceNs = nowNs;
            hasPreviousSource = true;
            return 0;
        }

        const uint64_t deltaNs = nowNs > previousSourceNs ? nowNs - previousSourceNs : 0;
        previousSourceNs = nowNs;

        // Menu pauses and swapchain transitions are not source cadence. Drop
        // accumulated credit so resuming cannot emit a burst of stale frames.
        constexpr uint64_t discontinuityNs = 250'000'000ULL;
        if (deltaNs == 0 || deltaNs > discontinuityNs) {
            generationCredit = 0.0L;
            return 0;
        }

        const long double desiredOutputFrames =
            static_cast<long double>(targetFps) * static_cast<long double>(deltaNs)
            / 1'000'000'000.0L;
        generationCredit += std::max(0.0L, desiredOutputFrames - 1.0L);

        constexpr long double roundingTolerance = 0.000001L;
        const auto available = static_cast<size_t>(
            std::floor(generationCredit + roundingTolerance));
        const size_t generated = std::min(maxGenerated, available);
        generationCredit -= static_cast<long double>(generated);
        if (generationCredit < 0.0L) generationCredit = 0.0L;
        return generated;
    }

private:
    bool enabled;
    uint32_t targetFps;
    size_t maxGenerated;
    bool hasPreviousSource{false};
    uint64_t previousSourceNs{0};
    long double generationCredit{0.0L};
};

/** Select evenly distributed LSFG outputs from a provisioned output set. */
inline std::vector<size_t> selectGeneratedFrameIndices(
        size_t provisionedCount, size_t requestedCount) {
    requestedCount = std::min(provisionedCount, requestedCount);
    std::vector<size_t> indices;
    indices.reserve(requestedCount);
    for (size_t i = 0; i < requestedCount; ++i) {
        const long double position =
            static_cast<long double>((i + 1) * (provisionedCount + 1))
            / static_cast<long double>(requestedCount + 1);
        indices.push_back(static_cast<size_t>(std::llround(position)) - 1);
    }
    return indices;
}
