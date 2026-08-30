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

    def test_android_pre_copy_has_real_completion_wait(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertNotIn(
            "Layer::ovkQueueSubmit(info.queue.second, 0, nullptr, VK_NULL_HANDLE)",
            source,
            "A zero-submit is not a completion wait and cannot synchronize the shared AHB across VkDevices",
        )
        self.assertIn("PFN_vkWaitForFences", source)
        self.assertIn("submitAndWaitForAhbHandoff", source)
        self.assertIn("waitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX)", source)

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

    def test_swapchain_context_initialization_has_staged_diagnostics(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        for marker in (
            "stage=swapchain-hook-enter",
            "stage=ls-context-begin",
            "stage=ls-context-ready",
            "stage=ls-context-failed",
        ):
            self.assertIn(marker, hooks)

    def test_runtime_config_change_is_reparsed_before_swapchain_recreation(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        present = hooks.split("VkResult myvkQueuePresentKHR", 1)[1]
        self.assertIn(
            "Config::updateConfig(",
            present,
            "A GameNative hot-reload must reparse conf.toml instead of remaining permanently OUT_OF_DATE",
        )
        self.assertIn(
            "Config::activeConf = Config::getConfig(Utils::getProcessName())",
            present,
        )
        self.assertIn("stage=config-reloaded", present)

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


if __name__ == "__main__":
    unittest.main()
