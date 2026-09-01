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
        self.assertIn("publishRuntimeState(configFile, false, false", hooks)
        self.assertIn("publishRuntimeState(activeConf.config_file, true, true", hooks)


if __name__ == "__main__":
    unittest.main()
