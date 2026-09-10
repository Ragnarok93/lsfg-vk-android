#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidAdaptiveHistoryContractTest(unittest.TestCase):
    def test_zero_generation_advances_history_without_source_pacing(self) -> None:
        header = (ROOT / "include/adaptive_scheduler.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertNotIn("delayUntilNextSourceOutput", header)
        self.assertNotIn("delayUntilNextSourceOutput", source)
        self.assertNotIn("std::this_thread::sleep_for(delay)", source)

        present_start = source.index("VkResult LsContext::present")
        handoff = source.index("submitAndWaitForAhbHandoff", present_start)
        adaptive_zero = source.index("if (adaptiveZeroGeneration)", handoff)
        zero_end = source.index("if (warmupSourceHistory)", adaptive_zero)
        zero_block = source[adaptive_zero:zero_end]

        self.assertGreater(adaptive_zero, handoff)
        self.assertIn("presentContextWithCount", zero_block)
        self.assertIn("stage=adaptive-history-advance", zero_block)
        self.assertIn("requiresSourceHistoryWarmup_ = false", zero_block)
        self.assertNotIn("requiresSourceHistoryWarmup_ = true", zero_block)
        self.assertNotIn("enterSourceOnlyBypass", zero_block)

    def test_source_only_bypass_remains_lifecycle_reset(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        bypass = source[source.index("void LsContext::enterSourceOnlyBypass"):]
        self.assertIn("requiresSourceHistoryWarmup_ = true", bypass)
        self.assertIn("previousSourceCopySignalValid_ = false", bypass)


if __name__ == "__main__":
    unittest.main()
