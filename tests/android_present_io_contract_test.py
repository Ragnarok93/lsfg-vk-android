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


if __name__ == "__main__":
    unittest.main()
