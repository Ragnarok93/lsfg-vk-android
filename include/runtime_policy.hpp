#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>

inline uint32_t saturatingOutputRate(uint32_t sourceFps, uint32_t multiplier) noexcept {
    if (sourceFps == 0 || multiplier <= 1)
        return 0;
    const uint64_t product = static_cast<uint64_t>(sourceFps)
        * static_cast<uint64_t>(multiplier);
    return product > std::numeric_limits<uint32_t>::max()
        ? std::numeric_limits<uint32_t>::max()
        : static_cast<uint32_t>(product);
}

// LSFG executes synchronously inside the application's present call. Output
// sleeping is therefore safe only when a separate source limiter has created a
// known frame-time budget. Fixed multiplier mode derives its cadence strictly
// from that source budget; Adaptive's target is an objective and is clamped to
// the maximum output rate the configured interpolation capacity can produce.
inline uint32_t resolveOutputPacingTarget(
        bool adaptive,
        uint32_t adaptiveTargetFps,
        uint32_t sourceFpsLimit,
        uint32_t multiplier) noexcept {
    const uint32_t capacityFps = saturatingOutputRate(sourceFpsLimit, multiplier);
    if (capacityFps == 0)
        return 0;
    if (!adaptive)
        return capacityFps;
    return std::min(adaptiveTargetFps, capacityFps);
}

// One app-visible WSI invalidation at a time. Settings written while a recreate
// is outstanding remain pending and are consumed together by the replacement
// swapchain instead of generating repeated VK_ERROR_OUT_OF_DATE_KHR storms.
class RuntimeWsiRecreateGate {
public:
    [[nodiscard]] bool inFlight() const noexcept {
        return inFlight_.load(std::memory_order_acquire);
    }

    bool requestIfNeeded(bool requiresRecreate) noexcept {
        if (!requiresRecreate)
            return false;
        bool expected = false;
        return inFlight_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    void onSwapchainCreation() noexcept {
        inFlight_.store(false, std::memory_order_release);
    }

private:
    std::atomic<bool> inFlight_{false};
};
