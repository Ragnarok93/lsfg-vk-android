#pragma once

#include <vulkan/vulkan_core.h>

#include <string_view>

namespace AndroidSyncPolicy {

enum class FramegenCompatibilityPath {
    AdrenoLatestKnownGood,
    XclipseCurrent,
    Generic,
};

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
/// Ordinary generated cycles hand the source upload to the private framegen
/// device with an OPAQUE_FD GPU semaphore, then use bounded host completion
/// before the game device consumes generated AHBs. Warmup, zero-generation
/// history, and true source-only recovery cycles retain the conservative
/// host-fence source-copy path where required. Generated frames and their
/// matching source are presented in the same intercepted call; deferred source
/// buffering and a synthetic game-device queue are not selected. This is a
/// driver-family policy, not a device-model allow/deny list.
///
/// Samsung/Xclipse, ARM/Mali, and unknown drivers retain their existing fully
/// capability-driven asynchronous synchronization.
inline FramegenCompatibilityPath selectFramegenCompatibilityPath(
        VkDriverId driverId, std::string_view driverName,
        std::string_view deviceName = {}) noexcept {
    if (driverId == VK_DRIVER_ID_MESA_TURNIP
            || driverId == VK_DRIVER_ID_QUALCOMM_PROPRIETARY) {
        return FramegenCompatibilityPath::AdrenoLatestKnownGood;
    }

    // VkPhysicalDeviceDriverProperties can be unavailable on older stacks.
    // Fall back to the reported driver/device name only in that case.
    if (containsAsciiCaseInsensitive(driverName, "turnip")
            || containsAsciiCaseInsensitive(driverName, "qualcomm")
            || containsAsciiCaseInsensitive(driverName, "adreno")
            || containsAsciiCaseInsensitive(deviceName, "qualcomm")
            || containsAsciiCaseInsensitive(deviceName, "adreno")) {
        return FramegenCompatibilityPath::AdrenoLatestKnownGood;
    }

    if (driverId == VK_DRIVER_ID_SAMSUNG_PROPRIETARY
            || containsAsciiCaseInsensitive(driverName, "xclipse")
            || containsAsciiCaseInsensitive(driverName, "samsung")
            || containsAsciiCaseInsensitive(deviceName, "xclipse")
            || containsAsciiCaseInsensitive(deviceName, "samsung")) {
        return FramegenCompatibilityPath::XclipseCurrent;
    }

    return FramegenCompatibilityPath::Generic;
}

inline bool requiresConservativeCrossDeviceSync(
        VkDriverId driverId, std::string_view driverName) noexcept {
    return selectFramegenCompatibilityPath(driverId, driverName)
        == FramegenCompatibilityPath::AdrenoLatestKnownGood;
}

inline const char* compatibilityPathName(
        FramegenCompatibilityPath path) noexcept {
    switch (path) {
    case FramegenCompatibilityPath::AdrenoLatestKnownGood:
        return "adreno-latest-known-good";
    case FramegenCompatibilityPath::XclipseCurrent:
        return "xclipse-current";
    case FramegenCompatibilityPath::Generic:
        return "generic-capability";
    }
    return "generic-capability";
}

inline const char* compatibilityVendorName(
        FramegenCompatibilityPath path) noexcept {
    switch (path) {
    case FramegenCompatibilityPath::AdrenoLatestKnownGood:
        return "Qualcomm";
    case FramegenCompatibilityPath::XclipseCurrent:
        return "Samsung";
    case FramegenCompatibilityPath::Generic:
        return "other";
    }
    return "other";
}

inline const char* crossDeviceSyncPolicyName(bool conservative) noexcept {
    return conservative
        ? "opaque-fd-input-host-completion-adreno"
        : "capability-async";
}

} // namespace AndroidSyncPolicy
