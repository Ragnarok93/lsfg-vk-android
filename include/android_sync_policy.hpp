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

/// Qualcomm/Adreno drivers use the compatibility path until the complete
/// cross-device SYNC_FD + AHardwareBuffer lifetime chain is validated on them.
/// This is a driver-family policy, not a device-model allow/deny list.
///
/// Samsung/Xclipse, ARM/Mali, and unknown drivers retain capability-driven
/// asynchronous synchronization so the compatibility repair does not become a
/// global performance rollback.
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
        ? "opaque-input-host-completion-adreno"
        : "capability-async";
}

} // namespace AndroidSyncPolicy
