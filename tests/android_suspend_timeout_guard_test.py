#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidSuspendTimeoutGuardTest(unittest.TestCase):
    def test_suspend_overshoot_gets_one_tiny_recheck_without_pacing_delay(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        for marker in (
            "framegenCompletionWaitElapsedNs",
            "framegenCompletionTimeoutNs * 2",
            "resumeCompletionRecheckNs = 2'000'000ULL",
            "framegen-completion-resume-recheck",
            "waitFramegenCompletion(resumeCompletionRecheckNs)",
        ):
            self.assertIn(marker, source)

        self.assertNotIn("sleep_for", source[source.index("const auto waitIdleStart"):source.index("// 4. Copy generated frames")])
        self.assertNotIn("delayUntilNextSourceOutput", source)

        wait_block = source[source.index("const auto waitIdleStart"):source.index("// 4. Copy generated frames")]
        self.assertEqual(wait_block.count("waitFramegenCompletion(resumeCompletionRecheckNs)"), 1)
        self.assertIn("if (!framegenReady && framegenCompletionWaitElapsedNs > framegenCompletionTimeoutNs * 2)", wait_block)
        self.assertIn("if (!framegenReady) {", wait_block)


if __name__ == "__main__":
    unittest.main()
