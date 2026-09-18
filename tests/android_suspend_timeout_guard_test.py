#!/usr/bin/env python3
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidSuspendTimeoutGuardTest(unittest.TestCase):
    def test_timeout_gets_one_fresh_bounded_recheck_without_pacing_delay(self) -> None:
        transform = ROOT / "scripts/adreno_suspend_timeout_guard.py"
        self.assertTrue(transform.exists())

        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            (temp_root / "src").mkdir(parents=True)
            shutil.copy2(ROOT / "src/context.cpp", temp_root / "src/context.cpp")
            subprocess.run(
                [sys.executable, str(transform), "--root", str(temp_root)],
                check=True,
            )
            source = (temp_root / "src/context.cpp").read_text(encoding="utf-8")

        for marker in (
            "framegenCompletionWaitElapsedNs",
            "resumeCompletionRecheckNs = 32'000'000ULL",
            "framegen-completion-resume-recheck",
            "waitFramegenCompletion(resumeCompletionRecheckNs)",
            "initial_wait_ms=",
        ):
            self.assertIn(marker, source)

        wait_block = source[
            source.index("const auto waitIdleStart"):
            source.index("// 4. Copy generated frames")
        ]
        self.assertNotIn("sleep_for", wait_block)
        self.assertNotIn("delayUntilNextSourceOutput", source)
        self.assertNotIn("framegenCompletionTimeoutNs * 2", wait_block)
        self.assertEqual(
            wait_block.count("waitFramegenCompletion(resumeCompletionRecheckNs)"),
            1,
        )
        self.assertIn("if (!framegenReady) {", wait_block)
        self.assertIn("if (!framegenReady) {", wait_block[wait_block.index("resumeCompletionRecheckNs"):])
        self.assertIn("framegenRecoveredAfterTimeout", wait_block)
        self.assertIn("if (!framegenRecoveredAfterTimeout)", wait_block)
        self.assertNotIn(
            "metrics.windowWaitIdleMs += std::chrono::duration<double, std::milli>(\n        RuntimeMetrics::Clock::now() - waitIdleStart).count();",
            wait_block,
        )


if __name__ == "__main__":
    unittest.main()
