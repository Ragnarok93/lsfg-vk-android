#include "android_sync_policy.hpp"

#include <cassert>

int main() {
    using AndroidSyncPolicy::FramegenCompatibilityPath;
    using AndroidSyncPolicy::crossDeviceSyncPolicyName;
    using AndroidSyncPolicy::requiresConservativeCrossDeviceSync;
    using AndroidSyncPolicy::selectFramegenCompatibilityPath;

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

    assert(selectFramegenCompatibilityPath(
        VK_DRIVER_ID_MESA_TURNIP, "Turnip")
        == FramegenCompatibilityPath::AdrenoLatestKnownGood);
    assert(selectFramegenCompatibilityPath(
        VK_DRIVER_ID_SAMSUNG_PROPRIETARY, "Samsung Xclipse")
        == FramegenCompatibilityPath::XclipseCurrent);
    assert(selectFramegenCompatibilityPath(
        static_cast<VkDriverId>(0), "unknown", "Adreno 650")
        == FramegenCompatibilityPath::AdrenoLatestKnownGood);
    assert(selectFramegenCompatibilityPath(
        static_cast<VkDriverId>(0), "unknown", "Xclipse 940")
        == FramegenCompatibilityPath::XclipseCurrent);
    assert(selectFramegenCompatibilityPath(
        static_cast<VkDriverId>(0), "unknown", "Generic GPU")
        == FramegenCompatibilityPath::Generic);

    assert(std::string_view(crossDeviceSyncPolicyName(true))
        == "adreno-source-protected-host-completion");
    assert(std::string_view(crossDeviceSyncPolicyName(false))
        == "capability-async");

    return 0;
}
