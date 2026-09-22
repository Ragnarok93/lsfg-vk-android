#pragma once

#include <vulkan/vulkan_core.h>

#include <string_view>

namespace AndroidSyncPolicy {

constexpr char asciiLower(char ch) noexcept {
    return ch >= 'A' && ch <= 'Z'
        ? static_cast<char>(ch - 'A' + 'a')
        : ch;
}

inline bool containsAsciiCaseInsensitive(
        std::string_view value, std::string_view needle) noexcept {
    if (needle.empty() || needle.size() > value.size())
        return false;

    for (std::size_t start = 0; start + needle.size() <= value.size(); ++start) {
        bool match = true;
        for (std::size_t offset = 0; offset < needle.size(); ++offset) {
            if (asciiLower(value[start + offset]) != asciiLower(needle[offset])) {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

/// Qualcomm/Adreno drivers use the validated protected source/history policy.
/// On single-queue Turnip, source input may use a one-shot SYNC_FD handoff, but
/// generated completion remains on the bounded host-completion path before the
/// game VkDevice touches output AHardwareBuffers. Cross-boundary deferred output
/// delivery is not device-proven on Adreno 650 and is intentionally disabled.
/// Zero-generation, reprime, and true source-only cycles remain source-safe.
/// This is a driver-family policy, not a device-model allow/deny list.
///
/// Samsung/Xclipse, ARM/Mali, and unknown drivers retain their existing fully
/// capability-driven asynchronous synchronization.
inline bool requiresConservativeCrossDeviceSync(
        VkDriverId driverId, std::string_view driverName) noexcept {
    if (driverId == VK_DRIVER_ID_MESA_TURNIP
            || driverId == VK_DRIVER_ID_QUALCOMM_PROPRIETARY) {
        return true;
    }

    // VkPhysicalDeviceDriverProperties can be unavailable on older stacks.
    // Fall back to the reported driver/device name only in that case.
    return containsAsciiCaseInsensitive(driverName, "turnip")
        || containsAsciiCaseInsensitive(driverName, "qualcomm")
        || containsAsciiCaseInsensitive(driverName, "adreno");
}

inline const char* crossDeviceSyncPolicyName(bool conservative) noexcept {
    return conservative
        ? "syncfd-input-host-completion-adreno"
        : "capability-async";
}

} // namespace AndroidSyncPolicy
