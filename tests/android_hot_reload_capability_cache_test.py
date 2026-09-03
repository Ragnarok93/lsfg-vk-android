#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# These contracts intentionally cover process-lifetime WSI residency and explicit
# fail-open swapchain state because both are prerequisites for safe runtime toggles.


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

    def test_targeted_process_keeps_wsi_interception_resident_while_generation_is_off(self) -> None:
        source = (ROOT / "src/layer_android.cpp").read_text(encoding="utf-8")

        self.assertIn(
            "bool shouldInterceptTarget()",
            source,
            "Android hot enable needs an immutable process-residency decision",
        )
        self.assertIn("Config::activeConf.targeted", source)
        self.assertNotIn(
            "Hooks::hooks.end() && Config::activeConf.enable",
            source,
            "Mutable generation state must not decide whether Vulkan returns LSFG WSI entrypoints",
        )

    def test_pass_through_swapchains_are_explicit_native_state(self) -> None:
        source = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")

        self.assertIn(
            "passThroughSwapchains",
            source,
            "Fail-open capability paths must be tracked instead of looking like missing swapchain state",
        )
        self.assertIn("capability_blocked=", source)
        self.assertIn("capability_reason=", source)
        self.assertIn("blit-unsupported", source)

    def test_performance_backend_toggle_requires_context_recreation(self) -> None:
        source = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        start = source.index("bool requiresSwapchainRecreation")
        end = source.index("bool supportsDeviceExtension", start)
        helper = source[start:end]

        self.assertIn(
            "previous.performance != next.performance",
            helper,
            "Performance mode selects LSFG_3_1 vs LSFG_3_1P at context construction and cannot mutate in place",
        )


if __name__ == "__main__":
    unittest.main()