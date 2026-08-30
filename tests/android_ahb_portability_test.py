#!/usr/bin/env python3
import re
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

    def test_android_pre_copy_has_real_completion_wait(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertNotIn(
            "Layer::ovkQueueSubmit(info.queue.second, 0, nullptr, VK_NULL_HANDLE)",
            source,
            "A zero-submit is not a completion wait and cannot synchronize the shared AHB across VkDevices",
        )
        self.assertRegex(
            source,
            re.compile(r"Layer::ovkWaitForFences|Layer::ovkQueueWaitIdle"),
            "Android AHB handoff must wait for the game-device copy before framegen consumes it",
        )

    def test_generated_ahb_is_not_treated_as_present_src(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        android = source.split("#ifdef __ANDROID__", 1)[1].split("#else", 1)[0]
        self.assertNotRegex(
            android,
            re.compile(r"copyImage\([^;]*out_n", re.DOTALL),
            "Generated AHB output needs an external-ownership-aware copy helper, not swapchain copyImage semantics",
        )


if __name__ == "__main__":
    unittest.main()
