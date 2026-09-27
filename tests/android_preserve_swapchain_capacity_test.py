#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidPreserveSwapchainCapacityTest(unittest.TestCase):
    def test_config_exposes_upstream_preserve_image_count_quirk(self) -> None:
        header = (ROOT / "include/config/config.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/config/config.cpp").read_text(encoding="utf-8")

        self.assertIn("bool preserveSwapchainImageCount{false};", header)
        self.assertIn('"preserve_swapchain_image_count"', source)
        self.assertIn('"LSFG_PRESERVE_SWAPCHAIN_IMAGE_COUNT"', source)

    def test_capacity_policy_preserves_native_count_explicitly_or_as_fallback(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")

        self.assertIn("preserveSwapchainImageCount", hooks)
        self.assertIn("capacityPolicy=", hooks)
        self.assertIn('"preserve-native-explicit"', hooks)
        self.assertIn('"preserve-native-fallback"', hooks)
        self.assertIn("createInfo.minImageCount = pCreateInfo->minImageCount;", hooks)

        android_capacity = hooks[
            hooks.index("bool preserveSwapchainImageCount ="):
            hooks.index("#else", hooks.index("bool preserveSwapchainImageCount ="))
        ]
        self.assertNotIn(
            'return createPassThrough("insufficient-headroom");',
            android_capacity,
        )

    def test_preserved_capacity_never_blocks_source_for_generated_image(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("bool preserveSwapchainImageCount", header)
        self.assertIn("bool preserveSwapchainImageCount_{false};", header)
        self.assertIn("this->preserveSwapchainImageCount_", source)
        self.assertRegex(
            source,
            r"generatedAcquireTimeoutNs\s*=\s*this->preserveSwapchainImageCount_\s*\?\s*0",
        )
        self.assertRegex(
            source,
            r"\(this->preserveSwapchainImageCount_\s*\|\|\s*!this->conservativeCrossDeviceSync_\)\s*&&\s*\(res == VK_NOT_READY \|\| res == VK_TIMEOUT\)",
        )

    def test_capacity_policy_change_recreates_swapchain(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")

        self.assertIn(
            "previous.preserveSwapchainImageCount != next.preserveSwapchainImageCount",
            hooks,
        )


if __name__ == "__main__":
    unittest.main()
