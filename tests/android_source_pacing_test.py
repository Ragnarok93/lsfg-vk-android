#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidSourcePacingContractTest(unittest.TestCase):
    def test_source_limit_is_distinct_from_adaptive_output_target(self) -> None:
        header = (ROOT / "include/config/config.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/config/config.cpp").read_text(encoding="utf-8")

        self.assertIn("uint32_t sourceFpsLimit{0};", header)
        self.assertIn('toml::find_or(gameTable, "source_fps_limit", 0U)', source)
        self.assertIn('std::getenv("LSFG_SOURCE_FPS_LIMIT")', source)
        self.assertIn("conf.sourceFpsLimit", source)

    def test_source_pacing_precedes_lsfg_present_and_interval_measurement(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        context = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn('#include "source_frame_pacer.hpp"', hooks)
        self.assertIn("SourceFramePacer sourceFramePacer", hooks)

        queue_present_start = hooks.index("VkResult myvkQueuePresentKHR")
        pacing_config = hooks.index(
            "sourceFramePacer.configure(conf.sourceFpsLimit)", queue_present_start
        )
        pacing_delay = hooks.index("sourceFramePacer.delayUntilNext", pacing_config)
        pacing_sleep = hooks.index("std::this_thread::sleep_for(sourcePacingDelay)", pacing_delay)
        lsfg_present = hooks.index("swapchain.present", pacing_sleep)

        context_present = context.index("VkResult LsContext::present")
        cycle_start = context.index("const auto cycleStart", context_present)
        adaptive_config = context.index("adaptiveScheduler_.configure", cycle_start)

        self.assertLess(pacing_config, pacing_delay)
        self.assertLess(pacing_delay, pacing_sleep)
        self.assertLess(pacing_sleep, lsfg_present)
        self.assertLess(cycle_start, adaptive_config)

    def test_source_limit_hot_reload_does_not_recreate_swapchain(self) -> None:
        source = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        helper_start = source.index("bool requiresSwapchainRecreation")
        helper_end = source.index("VkResult myvkCreateInstance", helper_start)
        helper = source[helper_start:helper_end]

        self.assertNotIn("sourceFpsLimit", helper)
        self.assertIn('"source_limit_fps="', source)


if __name__ == "__main__":
    unittest.main()
