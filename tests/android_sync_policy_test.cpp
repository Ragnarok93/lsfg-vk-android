#include "android_sync_policy.hpp"

#include <cassert>

int main() {
    using AndroidSyncPolicy::requiresConservativeCrossDeviceSync;

    assert(requiresConservativeCrossDeviceSync(
        VK_DRIVER_ID_MESA_TURNIP, "Turnip"));
    assert(requiresConservativeCrossDeviceSync(
        VK_DRIVER_ID_QUALCOMM_PROPRIETARY, "Qualcomm proprietary"));

    assert(requiresConservativeCrossDeviceSync(
        static_cast<VkDriverId>(0), "Turnip Mesa 25"));
    assert(requiresConservativeCrossDeviceSync(
        static_cast<VkDriverId>(0), "Adreno (TM) 650"));
    assert(requiresConservativeCrossDeviceSync(
        static_cast<VkDriverId>(0), "Qualcomm Vulkan"));

    assert(!requiresConservativeCrossDeviceSync(
        VK_DRIVER_ID_SAMSUNG_PROPRIETARY, "Samsung Xclipse"));
    assert(!requiresConservativeCrossDeviceSync(
        VK_DRIVER_ID_ARM_PROPRIETARY, "ARM Mali"));
    assert(!requiresConservativeCrossDeviceSync(
        static_cast<VkDriverId>(0), "Unknown Vulkan GPU"));

    return 0;
}
