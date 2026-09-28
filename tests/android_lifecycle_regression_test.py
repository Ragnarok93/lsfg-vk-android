#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidLifecycleRegressionTest(unittest.TestCase):
    def test_disabled_target_stays_resident_for_hot_enable(self) -> None:
        header = (ROOT / "include/config/config.hpp").read_text(encoding="utf-8")
        config = (ROOT / "src/config/config.cpp").read_text(encoding="utf-8")
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")

        self.assertIn("bool targeted{false}", header)
        self.assertGreaterEqual(config.count(".targeted = true"), 2)
        self.assertIn("!conf.targeted && !conf.enable", main)
        self.assertNotIn(
            'if (!conf.enable && name.second != "benchmark")',
            main,
        )

    def test_hot_reenable_keeps_application_present_mode(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")

        self.assertIn("const bool recreatingExistingSwapchain", hooks)
        self.assertIn("createInfo.presentMode = recreatingExistingSwapchain", hooks)
        self.assertIn("? pCreateInfo->presentMode", hooks)
        self.assertIn("stage=swapchain-blit-check-begin", hooks)
        self.assertIn("stage=swapchain-blit-check-ready", hooks)
        self.assertIn("stage=swapchain-downstream-create-begin", hooks)
        self.assertIn("stage=swapchain-downstream-create-return", hooks)

    def test_runtime_state_immediately_reports_generation_readiness(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")

        self.assertIn('"active="', hooks)
        self.assertIn('"generation_ready="', hooks)
        self.assertIn('"state="', hooks)
        self.assertIn('"resident="', hooks)
        self.assertIn('"source_only="', hooks)
        self.assertIn('"generation_initialized="', hooks)
        self.assertIn('"generated_presented="', hooks)
        self.assertIn('"degraded="', hooks)
        self.assertIn("publishRuntimeState", hooks)
        self.assertIn('publishRuntimeState(configFile, "degraded"', hooks)
        self.assertIn("const bool generationActive = activeConf.multiplier > 1", hooks)
        self.assertIn('generationActive ? "generating" : "source_only"', hooks)

    def test_resident_context_sizes_present_resources_to_runtime_capacity(self) -> None:
        """A 2x resident context must already own the slots needed by later 3x/4x hot toggles."""
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        allocation_start = source.index("for (size_t i = 0; i < 8; i++)")
        allocation_end = source.index("\n    }\n}", allocation_start)
        allocation = source[allocation_start:allocation_end]

        self.assertGreaterEqual(allocation.count("resize(runtimeMultiplier - 1)"), 5)
        self.assertNotIn("resize(conf.multiplier - 1)", allocation)


    def test_generation_off_preserves_native_present_mode(self) -> None:
        """LSFG-off must not inherit the frame-generation presentation policy."""
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")

        source_only_start = hooks.index("const auto createSourceOnly")
        source_only_end = hooks.index("if (!activeConf.enable)", source_only_start)
        source_only = hooks[source_only_start:source_only_end]

        self.assertIn(
            "sourceOnlyCreateInfo.presentMode = pCreateInfo->presentMode",
            source_only,
        )
        self.assertNotIn("choosePresentMode(", source_only)
        self.assertNotIn("VK_PRESENT_MODE_MAILBOX_KHR", source_only)


if __name__ == "__main__":
    unittest.main()
