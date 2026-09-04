#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidAhbPortabilityContractTest(unittest.TestCase):
    def test_game_side_ahb_import_prefers_android_buffer_properties(self) -> None:
        source = (ROOT / "src/mini/image.cpp").read_text(encoding="utf-8")
        self.assertIn(
            "Layer::ovkGetAndroidHardwareBufferPropertiesANDROID(device, ahbHandle",
            source,
            "Stock Android ICDs require the AHB import path to use the Android hardware-buffer properties contract",
        )
        self.assertIn("ahbProps.allocationSize", source)
        self.assertIn("ahbProps.memoryTypeBits", source)
        self.assertIn(
            "AHB properties query unavailable; using compatibility fallback",
            source,
            "Keep an explicit compatibility fallback for wrapper ICDs that do not expose the query",
        )

    def test_game_side_ahb_descriptor_only_requests_actual_gpu_usage(self) -> None:
        source = (ROOT / "src/mini/image.cpp").read_text(encoding="utf-8")
        android_ctor = source.split("#ifdef __ANDROID__\nImage::Image", 1)[1]
        descriptor = android_ctor.split("AHardwareBuffer_Desc ahbDesc", 1)[1].split("};", 1)[0]

        self.assertIn("AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE", descriptor)
        self.assertIn("AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT", descriptor)
        self.assertNotIn(
            "AHARDWAREBUFFER_USAGE_CPU_READ",
            descriptor,
            "The LSFG AHB path never CPU-locks these images; requiring CPU readability can make an otherwise valid GPU descriptor unallocatable on vendor gralloc implementations",
        )

    def test_game_side_ahb_copies_transfer_external_queue_ownership(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertGreaterEqual(
            source.count("VK_QUEUE_FAMILY_EXTERNAL"),
            4,
            "Game-side AHB input/output transfers must acquire from and release to external ownership",
        )
        self.assertIn("VK_IMAGE_LAYOUT_GENERAL", source)
        self.assertIn("VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL", source)
        self.assertIn("VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL", source)
        self.assertIn("copySwapchainToExternalAhb", source)
        self.assertIn("copyExternalAhbToSwapchain", source)

    def test_android_pre_copy_has_real_bounded_completion_wait(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertNotIn(
            "Layer::ovkQueueSubmit(info.queue.second, 0, nullptr, VK_NULL_HANDLE)",
            source,
            "A zero-submit is not a completion wait and cannot synchronize the shared AHB across VkDevices",
        )
        self.assertIn("PFN_vkWaitForFences", source)
        self.assertIn("submitAndWaitForAhbHandoff", source)
        self.assertIn("LSFG_VK_WAIT_TIMEOUT_MS", source)
        self.assertIn(
            "waitForFences(device, 1, &fence, VK_TRUE, runtimeWaitTimeoutNs())",
            source,
            "Cross-device AHB handoff must remain a real fence wait while using a finite timeout",
        )
        self.assertNotIn(
            "waitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX)",
            source,
            "A stuck ICD must not be able to block the frame-generation handoff forever",
        )

    def test_generated_ahb_uses_external_ownership_copy_path(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        present = source.split("VkResult LsContext::present", 1)[1]
        android_present = present.split("#ifdef __ANDROID__", 1)[1].split("#else", 1)[0]
        self.assertIn("copyExternalAhbToSwapchain", android_present)
        self.assertNotIn(
            "Utils::copyImage(",
            android_present,
            "Android generated AHB output must not use swapchain-only copyImage layout semantics",
        )

    def test_source_ahb_first_use_is_tracked_per_shared_image(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        present = source.split("VkResult LsContext::present", 1)[1]
        android_present = present.split("#ifdef __ANDROID__", 1)[1].split("#else", 1)[0]

        self.assertIn("sourceAhbInitialized_", header)
        self.assertIn("const size_t sourceAhbIndex", android_present)
        self.assertIn("const bool firstSourceAhbUse", android_present)
        self.assertNotIn(
            "this->frameIdx < 2",
            android_present,
            "Adaptive-zero direct presents may advance frameIdx before either source AHB has been initialized",
        )
        wait_pos = android_present.index("submitAndWaitForAhbHandoff(")
        init_pos = android_present.index(
            "this->sourceAhbInitialized_.at(sourceAhbIndex) = true;"
        )
        self.assertGreater(
            init_pos,
            wait_pos,
            "A source AHB becomes initialized only after its first copy/release handoff completes successfully",
        )

    def test_swapchain_context_initialization_has_staged_diagnostics(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        for marker in (
            "stage=swapchain-hook-enter",
            "stage=ls-context-begin",
            "stage=ls-context-ready",
            "stage=ls-context-failed",
        ):
            self.assertIn(marker, hooks)

    def test_runtime_config_change_is_staged_off_present_thread_before_wsi_restore(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        present = hooks.split("VkResult myvkQueuePresentKHR", 1)[1]

        self.assertIn(
            "applyPendingRuntimeConfig()",
            present,
            "The present thread should adopt an already-staged config snapshot only",
        )
        self.assertNotIn(
            "Config::updateConfig(",
            present,
            "No filesystem timestamp/config polling belongs in vkQueuePresentKHR",
        )
        self.assertNotIn(
            "Config::activeConf = Config::getConfig(Utils::getProcessName())",
            present,
            "Config parsing must remain on the reload worker, not the presentation hot path",
        )
        self.assertIn("stage=wsi-restore-requested", present)
        transition_present = present.index("Layer::ovkQueuePresentKHR(queue, pPresentInfo)")
        out_of_date = present.index("return VK_ERROR_OUT_OF_DATE_KHR")
        self.assertLess(
            transition_present,
            out_of_date,
            "A WSI-changing hot toggle must present the current real frame before requesting recreation",
        )

    def test_android_first_present_has_one_shot_framegen_stage_diagnostics(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        present = source.split("VkResult LsContext::present", 1)[1]
        android_present = present.split("#ifdef __ANDROID__", 1)[1].split("#else", 1)[0]
        self.assertIn("const bool firstPresentDiagnostic = this->frameIdx == 0;", android_present)
        for marker in (
            "runtime stage=first-present-enter",
            "runtime stage=source-ahb-handoff-ready",
            "runtime stage=framegen-dispatch-begin",
            "runtime stage=framegen-dispatch-returned",
            "runtime stage=framegen-idle-ready",
            "runtime stage=generated-present-ready",
            "runtime stage=first-present-cycle-ready",
        ):
            self.assertIn(marker, android_present)
        self.assertGreaterEqual(android_present.count("if (firstPresentDiagnostic)"), 6)

    def test_android_instance_preserves_game_extension_list(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        create_instance = hooks.split("VkResult myvkCreateInstance", 1)[1].split(
            "std::unordered_map<VkDevice, DeviceInfo>", 1
        )[0]
        android_path = create_instance.split("#ifdef __ANDROID__", 1)[1].split("#else", 1)[0]
        self.assertIn("Layer::ovkCreateInstance(pCreateInfo, pAllocator, pInstance)", android_path)
        self.assertNotIn("VK_KHR_external_memory_capabilities", android_path)
        self.assertNotIn("VK_KHR_external_semaphore_capabilities", android_path)
        self.assertNotIn("VK_KHR_get_physical_device_properties2", android_path)

    def test_missing_ahb_extension_fails_open_without_breaking_game_device(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        extension_dispatch = hooks.split("namespace Layer {", 1)[1].split("namespace {", 1)[0]
        self.assertIn("ovkEnumerateDeviceExtensionProperties", extension_dispatch)
        self.assertIn("ovkGetInstanceProcAddr(layerInstance", extension_dispatch)
        self.assertIn('"vkEnumerateDeviceExtensionProperties"', extension_dispatch)
        self.assertIn(
            "physicalDevice, nullptr, pPropertyCount, pProperties",
            extension_dispatch,
            "The capability probe must query the next Vulkan layer/ICD rather than recurse through this layer",
        )

        device_pre = hooks.split("VkResult myvkCreateDevicePre", 1)[1].split(
            "VkResult myvkCreateDevicePost", 1
        )[0]
        self.assertIn("supportsDeviceExtension(physicalDevice", device_pre)
        self.assertIn("VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME", device_pre)
        self.assertIn("stage=ahb-extension-unavailable", device_pre)
        self.assertIn("Layer::ovkCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice)", device_pre)
        self.assertIn("androidAhbSupported", hooks)

    def test_swapchain_size_is_multiplier_driven_not_queue_family_driven(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        swapchain = hooks.split("VkResult myvkCreateSwapchainKHR", 1)[1].split(
            "VkResult myvkQueuePresentKHR", 1
        )[0]
        self.assertNotIn(
            "static_cast<uint32_t>(deviceInfo.queue.first)",
            swapchain,
            "Queue-family numbering is vendor-specific and must never affect swapchain image count",
        )
        self.assertIn("requiredHeadroom", swapchain)
        self.assertIn("activeConf.multiplier - 1", swapchain)
        self.assertIn("pCreateInfo->minImageCount + requiredHeadroom", swapchain)
        self.assertIn("requiredImageCount > maxImageCount", swapchain)
        self.assertIn("stage=swapchain-insufficient-headroom", swapchain)
        self.assertNotIn(
            "pCreateInfo->minImageCount + 1",
            swapchain,
            "3x/4x modes require headroom derived from the configured generation multiplier",
        )

    def test_swapchain_transfer_usage_is_capability_gated_and_fails_open(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        swapchain = hooks.split("VkResult myvkCreateSwapchainKHR", 1)[1].split(
            "VkResult myvkQueuePresentKHR", 1
        )[0]
        self.assertIn("supportedUsageFlags", swapchain)
        self.assertIn("VK_IMAGE_USAGE_TRANSFER_SRC_BIT", swapchain)
        self.assertIn("VK_IMAGE_USAGE_TRANSFER_DST_BIT", swapchain)
        self.assertIn("stage=swapchain-unsupported-usage", swapchain)
        self.assertIn(
            "Layer::ovkCreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain)",
            swapchain,
            "Unsupported transfer usage must leave the game's swapchain untouched instead of failing creation",
        )


if __name__ == "__main__":
    unittest.main()
