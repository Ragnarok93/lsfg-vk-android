#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidPresentIoContractTest(unittest.TestCase):
    def test_runtime_stats_are_published_by_a_worker(self) -> None:
        source = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        self.assertIn("class AndroidStatsPublisher", source)
        self.assertIn("std::thread(&AndroidStatsPublisher::run, this)", source)
        self.assertIn("std::optional<Pending> pending_", source)
        self.assertIn("androidStatsPublisher.publish(runtimeStatsPath(configFile), out.str())", source)
        self.assertNotIn("dummyDeviceToInfoDeclarationGuard", source)

        stats_start = source.index("void writeRuntimeStatsFile")
        stats_end = source.index("void recordSuccessfulOutputCycle", stats_start)
        present_stats = source[stats_start:stats_end]
        self.assertNotIn("std::ofstream", present_stats)
        self.assertNotIn("std::filesystem::rename", present_stats)

    def test_disabled_present_is_zero_work_before_swapchain_lookup(self) -> None:
        source = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        self.assertIn("std::atomic<bool> androidFrameGenerationActive{false}", source)
        self.assertIn("bool consumeChanged() noexcept", source)
        self.assertIn("std::thread(&AndroidConfigWatcher::run, this)", source)
        self.assertNotIn("androidConfigWatcher.changed(", source)

        present_start = source.index("VkResult myvkQueuePresentKHR(")
        present_end = source.index("void myvkDestroySwapchainKHR", present_start)
        present = source[present_start:present_end]

        changed = present.index("androidConfigWatcher.consumeChanged()")
        inactive = present.index("!androidFrameGenerationActive.load")
        swapchain_lookup = present.index("swapchainToDeviceTable.find")
        context_lookup = present.index("swapchains.find")
        self.assertLess(changed, inactive)
        self.assertLess(inactive, swapchain_lookup)
        self.assertLess(swapchain_lookup, context_lookup)
        self.assertNotIn("std::filesystem::", present[:swapchain_lookup])
        self.assertNotIn("runtimeOutputStats[", present[:swapchain_lookup])

    def test_known_passthrough_swapchains_do_not_probe_lsfg_context(self) -> None:
        source = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        self.assertIn("std::unordered_set<VkSwapchainKHR> passThroughSwapchains", source)
        present_start = source.index("VkResult myvkQueuePresentKHR(")
        present_end = source.index("void myvkDestroySwapchainKHR", present_start)
        present = source[present_start:present_end]
        self.assertLess(
            present.index("passThroughSwapchains.find"),
            present.index("swapchains.find"),
        )


if __name__ == "__main__":
    unittest.main()
