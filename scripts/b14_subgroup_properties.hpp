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
    constexpr VkSubgroupFeatureFlags requiredOperations =
        VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT;
    return properties.subgroupSize >= 4U
        && (properties.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0
        && (properties.supportedOperations & requiredOperations)
            == requiredOperations;
}

// B15's earlier fusion is deliberately stricter than B14.  The rewrite
// addresses the sixteen mip4 producers as one contiguous subgroup tile; a
// merely large subgroup is not sufficient because subgroup topology is not
// portable across implementations.  The current SPIR-V pattern is defined
// for the 128-lane Adreno topology observed by the candidate and must remain
// disabled everywhere else until an equivalent lane mapping is proven.
[[nodiscard]] inline bool supportsSinglePassMipmaps(
        const VkPhysicalDeviceSubgroupProperties& properties) noexcept {
    constexpr VkSubgroupFeatureFlags requiredOperations =
        VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT;
    constexpr VkShaderStageFlags requiredStages = VK_SHADER_STAGE_COMPUTE_BIT;
    return properties.subgroupSize == 128U
        && (properties.supportedStages & requiredStages) == requiredStages
        && (properties.supportedOperations & requiredOperations)
            == requiredOperations;
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
