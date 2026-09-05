#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidCapabilityArchitectureContractTest(unittest.TestCase):
    def test_real_device_and_driver_uuid_replace_vendor_device_surrogate(self) -> None:
        utils = (ROOT / "src/utils/utils.cpp").read_text(encoding="utf-8")
        device = (ROOT / "framegen/src/core/device.cpp").read_text(encoding="utf-8")
        combined = utils + device
        self.assertIn("VkPhysicalDeviceIDProperties", combined)
        self.assertIn("deviceUUID", combined)
        self.assertIn("driverUUID", combined)
        self.assertNotIn("static_cast<uint64_t>(properties.vendorID) << 32", utils)
        self.assertNotIn("deviceUUID == 0x1463ABAC", device)

    def test_optional_device_extensions_are_probed_from_advertised_extensions(self) -> None:
        device = (ROOT / "framegen/src/core/device.cpp").read_text(encoding="utf-8")
        self.assertIn(
            "hasExtension(availableExtensions,\n        VK_EXT_ROBUSTNESS_2_EXTENSION_NAME)",
            device,
        )
        self.assertNotIn(
            "hasExtension(enabledExtensions,\n        VK_EXT_ROBUSTNESS_2_EXTENSION_NAME)",
            device,
        )

    def test_external_semaphore_fd_is_optional_and_capability_gated_on_android(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        device = (ROOT / "framegen/src/core/device.cpp").read_text(encoding="utf-8")
        backend = (ROOT / "framegen/public/lsfg_backend.hpp").read_text(encoding="utf-8")
        wrapper = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME", hooks)
        self.assertIn("supportsDeviceExtension(physicalDevice", hooks)
        self.assertIn('ovkGetDeviceProcAddr(*pDevice, "vkGetSemaphoreFdKHR")', hooks)
        self.assertIn("hasExternalSemaphoreFd", device)
        self.assertIn("vkImportSemaphoreFdKHR != nullptr", device)
        self.assertIn("bool externalSemaphoreFd{false}", backend)
        self.assertIn("info.androidExternalSemaphoreFdSupported", wrapper)
        self.assertIn("backendDiagnostics.externalSemaphoreFd", wrapper)
        self.assertNotIn("Adreno", wrapper)
        self.assertNotIn("Qualcomm", wrapper)

    def test_ahb_storage_contract_is_probed_and_allocated_correctly(self) -> None:
        image = (ROOT / "src/mini/image.cpp").read_text(encoding="utf-8")
        device = (ROOT / "framegen/src/core/device.cpp").read_text(encoding="utf-8")
        self.assertIn("AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER", image)
        self.assertIn("vkGetPhysicalDeviceImageFormatProperties2", device)
        self.assertIn("VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID", device)
        self.assertIn("VK_IMAGE_USAGE_STORAGE_BIT", device)
        self.assertIn("VK_IMAGE_USAGE_TRANSFER_SRC_BIT", device)
        self.assertIn("VK_IMAGE_USAGE_TRANSFER_DST_BIT", device)
        quality = (ROOT / "framegen/v3.1_src/context.cpp").read_text(encoding="utf-8")
        performance = (ROOT / "framegen/v3.1p_src/context.cpp").read_text(encoding="utf-8")
        for source in (quality, performance):
            self.assertIn("transportOnly", source)
            self.assertIn("sharedOutImages", source)
            self.assertIn("vkCmdCopyImage", source)
            self.assertIn("VK_IMAGE_USAGE_TRANSFER_SRC_BIT", source)
            self.assertIn("VK_IMAGE_USAGE_TRANSFER_DST_BIT", source)

    def test_swapchain_capacity_is_multiplier_driven_and_fails_open(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        self.assertIn("requiredHeadroom", hooks)
        self.assertIn("activeConf.multiplier", hooks)
        self.assertIn("swapchain-insufficient-headroom", hooks)
        self.assertNotIn("createInfo.minImageCount = pCreateInfo->minImageCount + 1;", hooks)

    def test_android_runtime_waits_are_bounded(self) -> None:
        wrapper = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        quality = (ROOT / "framegen/v3.1_src/context.cpp").read_text(encoding="utf-8")
        performance = (ROOT / "framegen/v3.1p_src/context.cpp").read_text(encoding="utf-8")
        quality_lifecycle = (ROOT / "framegen/v3.1_src/lsfg.cpp").read_text(encoding="utf-8")
        performance_lifecycle = (ROOT / "framegen/v3.1p_src/lsfg.cpp").read_text(encoding="utf-8")
        self.assertNotIn("UINT64_MAX", wrapper)
        self.assertNotIn("UINT64_MAX", quality)
        self.assertNotIn("UINT64_MAX", performance)
        self.assertNotIn("vkDeviceWaitIdle", quality_lifecycle)
        self.assertNotIn("vkDeviceWaitIdle", performance_lifecycle)

    def test_blit_capabilities_are_queried_before_use(self) -> None:
        combined = (
            (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
            + (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        )
        self.assertIn("vkGetPhysicalDeviceFormatProperties", combined)
        self.assertIn("VK_FORMAT_FEATURE_BLIT_SRC_BIT", combined)
        self.assertIn("VK_FORMAT_FEATURE_BLIT_DST_BIT", combined)

    def test_initialization_telemetry_contains_required_provenance_and_capacity(self) -> None:
        combined = (
            (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
            + (ROOT / "framegen/src/core/device.cpp").read_text(encoding="utf-8")
        )
        for token in (
            "driverUUID", "deviceUUID", "driverName", "driverVersion",
            "ahbR16fStorage", "externalSemaphoreFd", "requiredHeadroom",
            "minImageCount", "maxImageCount"
        ):
            self.assertIn(token, combined)


if __name__ == "__main__":
    unittest.main()
