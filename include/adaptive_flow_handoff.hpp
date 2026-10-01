#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace lsfg::handoff {

struct Identity {
    bool enabled{};
    bool targeted{};
    bool adaptiveFramegen{};
    bool adaptiveFlow{};
    bool performance{};
    bool hdr{};
    std::size_t multiplier{};
    std::uint32_t targetFps{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::string_view dll;
    std::string_view preset;
};

[[nodiscard]] inline bool compatible(
        const Identity& oldState, const Identity& newState) noexcept {
    return oldState.enabled && newState.enabled
        && oldState.targeted == newState.targeted
        && oldState.adaptiveFramegen == newState.adaptiveFramegen
        && oldState.adaptiveFlow && newState.adaptiveFlow
        && oldState.performance == newState.performance
        && oldState.hdr == newState.hdr
        && oldState.multiplier == newState.multiplier
        && oldState.targetFps == newState.targetFps
        && oldState.width == newState.width
        && oldState.height == newState.height
        && oldState.dll == newState.dll
        && oldState.preset == newState.preset;
}

struct StartupPlan {
    float backendInitialScale{1.0F};
    std::optional<float> postCreateScale;
};

[[nodiscard]] inline StartupPlan planAdaptiveStartup(
        float presetTargetScale, float requestedSeedScale) noexcept {
    StartupPlan plan{};
    if (!std::isfinite(presetTargetScale)
            || presetTargetScale < 0.25F
            || presetTargetScale > 1.0F) {
        return plan;
    }

    plan.backendInitialScale = presetTargetScale;
    if (std::isfinite(requestedSeedScale)
            && requestedSeedScale >= 0.25F
            && requestedSeedScale <= presetTargetScale
            && std::fabs(requestedSeedScale - presetTargetScale) > 0.0005F) {
        plan.postCreateScale = requestedSeedScale;
    }
    return plan;
}

[[nodiscard]] inline bool startupSeedHistoryOnly(
        bool adaptiveFramegen,
        bool startupSeedPending,
        bool transitionPending,
        std::uint32_t warmupRemaining) noexcept {
    return adaptiveFramegen
        && startupSeedPending
        && transitionPending
        && warmupRemaining > 0;
}

[[nodiscard]] inline std::optional<float> selectEnableScale(
        const Identity& oldState, const Identity& newState,
        float configuredScale) noexcept {
    if (!oldState.enabled || !newState.enabled
            || oldState.adaptiveFlow || !newState.adaptiveFlow
            || !newState.adaptiveFramegen
            || oldState.targeted != newState.targeted
            || oldState.adaptiveFramegen != newState.adaptiveFramegen
            || oldState.performance != newState.performance
            || oldState.hdr != newState.hdr
            || oldState.multiplier != newState.multiplier
            || oldState.targetFps != newState.targetFps
            || oldState.width != newState.width
            || oldState.height != newState.height
            || oldState.dll != newState.dll
            || !std::isfinite(configuredScale)
            || configuredScale < 0.25F
            || configuredScale > 1.0F) {
        return std::nullopt;
    }
    return configuredScale;
}

[[nodiscard]] inline std::optional<float> selectStableScale(
        bool oldRuntimeEnabled,
        const Identity& oldState,
        const Identity& newState,
        float requestedScale,
        float activeScale,
        bool transitionPending,
        std::uint32_t warmupRemaining) noexcept {
    constexpr float kScaleEpsilon = 0.0005F;
    if (!oldRuntimeEnabled
            || !compatible(oldState, newState)
            || transitionPending
            || warmupRemaining != 0
            || !std::isfinite(requestedScale)
            || !std::isfinite(activeScale)
            || std::fabs(requestedScale - activeScale) > kScaleEpsilon) {
        return std::nullopt;
    }
    return activeScale;
}

} // namespace lsfg::handoff
