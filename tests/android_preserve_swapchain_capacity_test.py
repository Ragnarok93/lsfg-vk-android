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

    def test_native_capacity_reuses_existing_nonblocking_generic_wsi_policy(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        # The quirk is swapchain-only. Xclipse/generic already use zero-time
        # generated acquisition, so no presentation/synchronization rewrite is
        # required to make native capacity source-safe.
        self.assertIn(
            "this->conservativeCrossDeviceSync_\n"
            "                ? runtimeWaitTimeoutNs()\n"
            "                : 0",
            source,
        )
        self.assertIn(
            "!this->conservativeCrossDeviceSync_\n"
            "                && (res == VK_NOT_READY || res == VK_TIMEOUT)",
            source,
        )
        self.assertNotIn("preserveSwapchainImageCount_", source)

    def test_capacity_policy_change_recreates_swapchain(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")

        self.assertIn(
            "previous.preserveSwapchainImageCount != next.preserveSwapchainImageCount",
            hooks,
        )

    def test_protected_adreno_keeps_september18_capacity_contract(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")

        self.assertIn("protectedAdrenoCapacityPath", hooks)
        self.assertIn('"legacy-headroom-adreno-protected"', hooks)
        self.assertIn('"action=pass-through-adreno-protected"', hooks)
        self.assertIn('"reason=adreno-protected-364178af"', hooks)

        android_capacity = hooks[
            hooks.index("const bool protectedAdrenoCapacity ="):
            hooks.index("#else", hooks.index("const bool protectedAdrenoCapacity ="))
        ]
        self.assertLess(
            android_capacity.index("protectedAdrenoCapacity && preserveSwapchainImageCount"),
            android_capacity.index("preserve-native-fallback"),
        )
        self.assertIn(
            'headroomOverflow ? "headroom-overflow" : "insufficient-headroom"',
            android_capacity,
        )


if __name__ == "__main__":
    unittest.main()
