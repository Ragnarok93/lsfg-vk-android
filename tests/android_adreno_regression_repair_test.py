#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidAdrenoRegressionRepairTest(unittest.TestCase):
    def test_adreno_historical_async_attempt_is_not_blocked_by_opaque_capability_bit(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        start = source.index("const bool syncFdHandoffSupported")
        end = source.index("const bool xclipseCompatibilityPath", start)
        selection = source[start:end]

        self.assertIn("adrenoHistoricalOpaqueAttempt", selection)
        self.assertIn("info.androidSyncFdSemaphoreSupported", selection)
        self.assertIn("backendDiagnostics.externalSemaphoreSyncFd", selection)
        self.assertIn("opaqueFdHandoffSupported || adrenoHistoricalOpaqueAttempt", selection)
        self.assertIn(
            "VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT",
            selection,
        )

    def test_adreno_admission_rejection_escapes_before_ahb_copy(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        begin = source.index("// BEGIN ADRENO_364178AF_EXECUTION")
        end = source.index("// END ADRENO_364178AF_EXECUTION", begin)
        adreno = source[begin:end]

        escape = adreno.index("if (conservativeAdmissionRejectedHistoryGap)")
        first_copy = adreno.index("copySwapchainToExternalAhb", escape)
        self.assertLess(escape, first_copy)

        escape_block = adreno[escape:first_copy]
        self.assertIn("SourceCadenceObservation::SourceOnly", escape_block)
        self.assertIn("gameRenderSemaphores", escape_block)
        self.assertIn("sourceHistoryWarmupRemaining_ = 1", escape_block)
        self.assertIn("requiresSourceHistoryWarmup_ = true", escape_block)
        self.assertIn("game-render-admission-bypass", escape_block)
        self.assertNotIn("presentContextWithCount(", escape_block)
        self.assertNotIn("submitAndWaitForAhbHandoff(", escape_block)


if __name__ == "__main__":
    unittest.main()
