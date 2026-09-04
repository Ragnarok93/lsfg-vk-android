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
        self.assertIn("publishRuntimeState", hooks)
        self.assertIn(
            "publishRuntimeState(activeConf.config_file, false, false",
            hooks,
            "Pass-through/fail-open swapchains must immediately publish generation_ready=0 from the active cached config",
        )
        self.assertIn(
            "publishRuntimeState(activeConf.config_file, true, true",
            hooks,
            "A successfully constructed LSFG swapchain must immediately publish generation_ready=1",
        )

    def test_fractional_adaptive_zero_keeps_source_history_live(self) -> None:
        """A fractional 0/1 cadence must not turn every planned 1 into warmup."""
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        zero_start = source.index("if (generatedFrameCount == 0)")
        history_path = source.index("// Android path: AHardwareBuffer exchange", zero_start)
        zero_fast_path = source[zero_start:history_path]

        self.assertNotIn(
            "requiresSourceHistoryWarmup_ = true",
            zero_fast_path,
            "An intentional fractional zero is not a discontinuity",
        )
        self.assertNotIn(
            "previousSourceCopySignalValid_ = false",
            zero_fast_path,
            "Fractional zero cadence must not discard valid source-copy ordering",
        )
        self.assertIn("preserveAdaptiveSourceHistory", source)
        self.assertIn("stage=source-history-maintained", source)


if __name__ == "__main__":
    unittest.main()
