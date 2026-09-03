#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidHotReloadCapabilityCacheTest(unittest.TestCase):
    def test_successful_blit_capability_is_reused_across_wsi_recreation(self) -> None:
        source = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        helper_start = source.index("bool supportsBidirectionalBlit")
        helper_end = source.index("VkResult myvkCreateSwapchainKHR", helper_start)
        helper = source[helper_start:helper_end]

        self.assertIn("blitCapabilityCache", source)
        self.assertIn("blitCapabilityMutex", source)
        self.assertIn("blitCapabilityCache.find", helper)
        self.assertIn("blitCapabilityCache.emplace", helper)
        self.assertLess(
            helper.index("blitCapabilityCache.find"),
            helper.index("getFormatProperties(physicalDevice, sharedFormat"),
            "Hot WSI recreation must reuse the stable per-device format capability before touching instance dispatch again",
        )


if __name__ == "__main__":
    unittest.main()
