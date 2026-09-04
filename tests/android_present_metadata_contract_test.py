#!/usr/bin/env python3
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
CONTEXT = (ROOT / "src/context.cpp").read_text()

class PresentMetadataContractTest(unittest.TestCase):
    def test_generated_presents_do_not_reuse_game_present_metadata(self):
        self.assertNotIn(".pNext = i == 0 ? pNext : nullptr,", CONTEXT)
        self.assertGreaterEqual(CONTEXT.count(".pNext = nullptr,"), 2)

    def test_real_source_present_keeps_game_present_metadata(self):
        marker = "const VkPresentInfoKHR finalPresentInfo{"
        start = CONTEXT.index(marker)
        block = CONTEXT[start:start + 700]
        self.assertIn(".pNext = pNext,", block)
        self.assertNotIn("generatedFrameCount == 0 ? pNext", block)

if __name__ == "__main__":
    unittest.main()
