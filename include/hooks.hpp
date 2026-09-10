#pragma once

#include <vulkan/vulkan_core.h>
#include "lsfg_backend.hpp"

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <string>

namespace Hooks {

    /// Vulkan device information structure.
    struct DeviceInfo {
        VkDevice device;
        VkPhysicalDevice physicalDevice;
        LSFG::DeviceIdentity identity{};
        bool identityValid{false};
        std::pair<uint32_t, VkQueue> queue; // graphics family
        bool androidAhbSupported{true};
        // Optional Android optimization. False preserves the established
        // synchronous AHB fence handoff with no change in presentation behavior.
        bool androidOpaqueFdSemaphoreSupported{false};
    };

    /// Map of hooked Vulkan functions.
    extern std::unordered_map<std::string, PFN_vkVoidFunction> hooks;

}