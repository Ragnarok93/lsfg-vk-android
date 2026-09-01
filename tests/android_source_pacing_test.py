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

    def test_source_pacing_precedes_source_interval_measurement(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn('#include "source_frame_pacer.hpp"', header)
        self.assertIn("SourceFramePacer sourceFramePacer_", header)

        present_start = source.index("VkResult LsContext::present")
        android_start = source.index("#ifdef __ANDROID__", present_start)
        pacing_config = source.index(
            "sourceFramePacer_.configure(conf.sourceFpsLimit)", android_start
        )
        pacing_delay = source.index("sourceFramePacer_.delayUntilNext", pacing_config)
        pacing_sleep = source.index("std::this_thread::sleep_for(sourcePacingDelay)", pacing_delay)
        cycle_start = source.index("const auto cycleStart", pacing_sleep)
        adaptive_config = source.index("adaptiveScheduler_.configure", cycle_start)

        self.assertLess(pacing_config, pacing_delay)
        self.assertLess(pacing_delay, pacing_sleep)
        self.assertLess(pacing_sleep, cycle_start)
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
