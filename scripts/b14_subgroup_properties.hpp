#pragma once

#include <vulkan/vulkan_core.h>

#include <cstdint>

namespace b14 {

enum class SubgroupQueryRoute : uint8_t {
    Unavailable,
    Core,
    Khr,
};

struct SubgroupQueryResult {
    VkPhysicalDeviceSubgroupProperties properties{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES,
    };
    SubgroupQueryRoute route{SubgroupQueryRoute::Unavailable};
};

[[nodiscard]] inline bool supportsCooperativeMipmaps(
        const VkPhysicalDeviceSubgroupProperties& properties) noexcept {
    // B14 assumes subgroup-local lanes 0..3 correspond to the first four
    // workgroup-local producers. Vulkan does not guarantee any direct mapping
    // between SubgroupLocalInvocationId and LocalInvocationId/Index unless
    // full-subgroup execution semantics are explicitly established for the
    // pipeline. This runtime does not currently establish that contract.
    //
    // Fail closed to the B13 shader until B14 is rewritten around a guaranteed
    // local-index mapping or the pipeline explicitly requires full subgroups.
    (void)properties;
    return false;
}

[[nodiscard]] inline bool hasCompleteSubgroupMetadata(
        const VkPhysicalDeviceSubgroupProperties& properties) noexcept {
    return properties.subgroupSize != 0U
        && properties.supportedStages != 0
        && properties.supportedOperations != 0;
}

inline void queryOne(VkPhysicalDevice physicalDevice,
        PFN_vkGetPhysicalDeviceProperties2 query,
        VkPhysicalDeviceSubgroupProperties& subgroupProperties) {
    subgroupProperties = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES,
    };
    VkPhysicalDeviceProperties2 properties{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &subgroupProperties,
    };
    query(physicalDevice, &properties);
}

[[nodiscard]] inline SubgroupQueryResult querySubgroupProperties(
        VkPhysicalDevice physicalDevice,
        PFN_vkGetPhysicalDeviceProperties2 coreQuery,
        PFN_vkGetPhysicalDeviceProperties2KHR khrQuery) {
    SubgroupQueryResult core{};
    if (coreQuery != nullptr) {
        queryOne(physicalDevice, coreQuery, core.properties);
        core.route = SubgroupQueryRoute::Core;
        if (supportsCooperativeMipmaps(core.properties))
            return core;
    }

    SubgroupQueryResult khr{};
    if (khrQuery != nullptr) {
        queryOne(physicalDevice,
            reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(khrQuery),
            khr.properties);
        khr.route = SubgroupQueryRoute::Khr;
        if (supportsCooperativeMipmaps(khr.properties)
                || coreQuery == nullptr
                || (!hasCompleteSubgroupMetadata(core.properties)
                    && hasCompleteSubgroupMetadata(khr.properties)))
            return khr;
    }

    return core;
}

[[nodiscard]] inline const char* subgroupQueryRouteName(
        SubgroupQueryRoute route) noexcept {
    switch (route) {
        case SubgroupQueryRoute::Core: return "core";
        case SubgroupQueryRoute::Khr: return "khr";
        case SubgroupQueryRoute::Unavailable: return "unavailable";
    }
    return "unavailable";
}

} // namespace b14
