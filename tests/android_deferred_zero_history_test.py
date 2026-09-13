#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidDeferredZeroHistoryContractTest(unittest.TestCase):
    def test_candidate_a_declares_conservative_history_state_machine(self) -> None:
        transform = ROOT / "scripts/adreno_deferred_zero_history.py"
        self.assertTrue(transform.exists(), "missing Candidate A deferred-zero transform")
        text = transform.read_text(encoding="utf-8")

        for marker in (
            "HistoryMaintenanceState",
            "LiveHistory",
            "DeferredZero",
            "ReprimeHistory",
            "kDeferredZeroDecisionThreshold = 6",
            "kDeferredZeroMinimumDurationMs = 250",
            "kRawHistoryBudgetBytes = 64ULL * 1024ULL * 1024ULL",
            "std::array<Mini::Image, 3> rawSourceHistory_",
            "framegenHistoryEpoch_",
        ):
            self.assertIn(marker, text)

    def test_deferred_zero_uses_local_three_frame_ring_and_no_framegen_handoff(self) -> None:
        transform = (ROOT / "scripts/adreno_deferred_zero_history.py").read_text(encoding="utf-8")
        for marker in (
            "DeviceLocalImageTag",
            "copySwapchainToRawHistory",
            "rawSourceHistoryNext_",
            "rawSourceHistoryCount_",
            "deferred-zero state=enter",
            "deferred-zero source-only no-framegen",
            "presentDeferredZeroSourceOnly",
        ):
            self.assertIn(marker, transform)

        deferred_start = transform.index("deferred-zero source-only no-framegen")
        deferred_end = transform.index("deferred-zero reprime-begin", deferred_start)
        deferred_block = transform[deferred_start:deferred_end]
        self.assertNotIn("presentContextWithCount", deferred_block)
        self.assertNotIn("submitAhbHandoff", deferred_block)
        self.assertNotIn("submitAndWaitForAhbHandoff", deferred_block)

    def test_reprime_replays_oldest_to_newest_and_consumes_one_source_only_cycle(self) -> None:
        transform = (ROOT / "scripts/adreno_deferred_zero_history.py").read_text(encoding="utf-8")
        for marker in (
            "deferred-zero reprime-begin",
            "orderedRawHistorySlots",
            "for (size_t replayIndex = 0; replayIndex < 3; ++replayIndex)",
            "copyRawHistoryToExternalAhb",
            "presentContextWithCountAndHistoryFd",
            "framegenHistoryEpoch_++",
            "deferred-zero reprime-complete source_only=1",
            "lastGeneratedFrameCount_ = 0",
        ):
            self.assertIn(marker, transform)

    def test_reprime_uses_normal_game_to_framegen_sync_fd_handoff(self) -> None:
        sync_transform = ROOT / "scripts/adreno_deferred_zero_reprime_sync.py"
        self.assertTrue(sync_transform.exists(), "missing synchronized re-prime transform")
        reprime = sync_transform.read_text(encoding="utf-8")

        for marker in (
            "Mini::Semaphore replayInputSemaphore",
            "VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT",
            "replayInputSemaphore.exportFd",
            "replayInputSemaphoreFd",
            "presentContextWithCountAndHistoryFd",
            "deferred-zero reprime input-sync-fd",
            "asyncZeroHistoryEnabled_",
        ):
            self.assertIn(marker, reprime)

        self.assertIn("{ replayInputSemaphore.handle() }", reprime)
        self.assertNotIn("*this->lsfgCtxId, -1, noOutSems, 0", reprime)

        bundle = (ROOT / "scripts/apply-adreno-evidence-profile.py").read_text(encoding="utf-8")
        self.assertIn("apply_deferred_zero_reprime_sync(root)", bundle)
        self.assertLess(
            bundle.index("apply_deferred_zero_history(root)"),
            bundle.index("apply_deferred_zero_reprime_sync(root)"),
        )
        self.assertLess(
            bundle.index("apply_deferred_zero_reprime_sync(root)"),
            bundle.index("apply_deferred_zero_history_finalize(root)"),
        )

    def test_failure_and_lifecycle_paths_fall_back_without_changing_scheduler(self) -> None:
        transform = (ROOT / "scripts/adreno_deferred_zero_history.py").read_text(encoding="utf-8")
        for marker in (
            "deferredZeroDisabledForContext_",
            "raw-history-allocation-fallback",
            "reprime-fallback",
            "invalidateDeferredZeroHistory",
            "adaptiveTelemetry.discontinuityReset",
            "enterSourceOnlyBypass",
        ):
            self.assertIn(marker, transform)

        self.assertNotIn("AdaptiveFrameScheduler::plan", transform)
        self.assertNotIn("src/adaptive_scheduler.cpp", transform)

    def test_candidate_a_is_applied_after_existing_slot_release_overlap(self) -> None:
        bundle = (ROOT / "scripts/apply-adreno-evidence-profile.py").read_text(encoding="utf-8")
        self.assertIn("apply_deferred_zero_history(root)", bundle)
        self.assertLess(
            bundle.index("apply_transport_release_overlap(root)"),
            bundle.index("apply_deferred_zero_history(root)"),
            "Candidate A must patch the already slot-aware/release-overlapped Android source",
        )

    def test_candidate_a_checkpoint_reports_reprime_time_and_preserves_present_result(self) -> None:
        finalize = ROOT / "scripts/adreno_deferred_zero_history_finalize.py"
        self.assertTrue(finalize.exists(), "missing Candidate A checkpoint finalization transform")
        text = finalize.read_text(encoding="utf-8")
        self.assertIn("deferred_zero_reprime_time_ms", text)
        self.assertIn("deferredReprimeStart", text)
        self.assertIn("finishResult", text)
        self.assertIn("VK_ERROR_OUT_OF_DATE_KHR", text)

        bundle = (ROOT / "scripts/apply-adreno-evidence-profile.py").read_text(encoding="utf-8")
        self.assertIn("apply_deferred_zero_history_finalize(root)", bundle)
        self.assertLess(
            bundle.index("apply_deferred_zero_history(root)"),
            bundle.index("apply_deferred_zero_history_finalize(root)"),
        )


if __name__ == "__main__":
    unittest.main()
