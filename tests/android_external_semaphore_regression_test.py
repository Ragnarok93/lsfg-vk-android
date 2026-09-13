#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidExternalSemaphoreRegressionTest(unittest.TestCase):
    def test_input_semaphore_does_not_require_output_semaphore_fds(self) -> None:
        """GPU input handoff is valid even when Android uses fence-based output completion."""
        for relative in (
            "framegen/v3.1_src/context.cpp",
            "framegen/v3.1p_src/context.cpp",
        ):
            source = (ROOT / relative).read_text(encoding="utf-8")
            self.assertNotIn(
                "if (inSem >= 0) outSemaphore = Core::Semaphore(vk.device, outSem.empty() ? -1 : outSem.at(pass));",
                source,
                f"{relative}: an input semaphore must not force import of a synthetic -1 output fd",
            )
            self.assertIn(
                "pass < outSem.size()",
                source,
                f"{relative}: output semaphore import must be gated by an actual output fd",
            )
            self.assertIn(
                "outSem.at(pass) >= 0",
                source,
                f"{relative}: negative output fd sentinels must never reach Core::Semaphore(fd)",
            )

    def test_adaptive_zero_generation_has_reverse_sync_fd_contract(self) -> None:
        """Zero-generation history preprocessing must complete GPU-to-GPU, not by an immediate host wait."""
        transform = (ROOT / "scripts/adreno_syncfd_handoff.py").read_text(encoding="utf-8")

        self.assertIn("historyCompletionFd", transform)
        self.assertIn("pendingHistoryCompletionSemaphore_", transform)
        self.assertIn("importFd", transform)
        self.assertIn("adaptiveZeroGeneration", transform)
        self.assertIn("zero-history-sync-fd", transform)
        self.assertIn("preprocessingPending", transform)


if __name__ == "__main__":
    unittest.main()
