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

    def test_reprime_exit_uses_three_real_source_history_advances(self) -> None:
        safe = ROOT / "scripts/adreno_deferred_zero_safe_reprime.py"
        self.assertTrue(safe.exists(), "missing safe DeferredZero re-prime transform")
        text = safe.read_text(encoding="utf-8")

        for marker in (
            "kDeferredZeroWarmupFrames = 3",
            "deferredZeroWarmupFramesRemaining_",
            "deferredReprimeStart_",
            "deferredReprimeWarmup",
            "adaptiveZeroGeneration || deferredReprimeWarmup",
            "deferred-zero reprime-warmup-begin",
            "deferred-zero reprime-warmup-progress",
            "deferred-zero reprime-complete source_only=1",
            "framegenHistoryEpoch_++",
        ):
            self.assertIn(marker, text)

        # The crashing raw-history -> shared-AHB replay must not survive into
        # the final transformed source. Re-prime is performed by the existing
        # proven source->AHB zero-history maintenance path over real presents.
        self.assertIn("reprime_start_marker", text)
        self.assertIn("normal_history_marker", text)
        self.assertNotIn("copyRawHistoryToExternalAhb(", text)
        self.assertNotIn("replayInputSemaphore", text)

    def test_deferred_zero_exit_requires_dense_scheduler_requests_before_reprime(self) -> None:
        gate = ROOT / "scripts/adreno_deferred_zero_exit_persistence.py"
        self.assertTrue(gate.exists(), "missing DeferredZero exit-persistence transform")
        text = gate.read_text(encoding="utf-8")

        for marker in (
            "kDeferredZeroWakeRequestThreshold = 2",
            "kDeferredZeroWakeRequestWindowMs = 500",
            "deferredZeroWakeRequestCount_",
            "deferredZeroWakeWindowStart_",
            "deferred-zero reprime-deferred",
            "deferred-zero reprime-warmup-begin",
            "historyMaintenanceState_ == HistoryMaintenanceState::DeferredZero",
            "generatedFrameCount > 0",
        ):
            self.assertIn(marker, text)

        # The bridge may delay an expensive history wake, but it must not
        # rewrite the scheduler policy itself. While the request-density gate
        # is pending, DeferredZero remains on the source-only raw-ring path.
        self.assertNotIn("AdaptiveFrameScheduler::plan", text)
        self.assertNotIn("src/adaptive_scheduler.cpp", text)
        self.assertIn("if (this->historyMaintenanceState_ == HistoryMaintenanceState::DeferredZero) {", text)
        self.assertIn("if (adaptiveZeroGeneration) {", text)

        bundle = (ROOT / "scripts/apply-adreno-evidence-profile.py").read_text(encoding="utf-8")
        self.assertIn("apply_deferred_zero_exit_persistence", bundle)
        self.assertIn("apply_deferred_zero_exit_persistence(root)", bundle)
        self.assertLess(
            bundle.index("apply_deferred_zero_safe_reprime(root)"),
            bundle.index("apply_deferred_zero_exit_persistence(root)"),
            "exit-persistence gate must compose after the validated safe live re-prime transform",
        )

    def test_safe_reprime_lifecycle_reset_is_method_bounded(self) -> None:
        safe = (ROOT / "scripts/adreno_deferred_zero_safe_reprime.py").read_text(encoding="utf-8")
        self.assertIn("invalidate_start_marker", safe)
        self.assertIn("ordered_slots_marker", safe)
        self.assertIn("invalidate_start = text.find", safe)
        self.assertIn("ordered_slots_start = text.find", safe)
        self.assertNotIn(
            '"    this->zeroDemandStart_ = {};\\n}\\n#endif\\n\\n"',
            safe,
            "safe re-prime must not depend on preprocessor adjacency around invalidateDeferredZeroHistory",
        )

    def test_safe_reprime_cpp_templates_preserve_escape_sequences(self) -> None:
        safe = (ROOT / "scripts/adreno_deferred_zero_safe_reprime.py").read_text(encoding="utf-8")
        self.assertIn("new_transition = r'''", safe)
        self.assertIn("new_counters = r'''", safe)
        self.assertIn("<< '\\n';", safe)

    def test_safe_reprime_supersedes_legacy_replay_after_checkpoint_transforms(self) -> None:
        bundle = (ROOT / "scripts/apply-adreno-evidence-profile.py").read_text(encoding="utf-8")
        self.assertIn("apply_deferred_zero_safe_reprime", bundle)
        self.assertIn("apply_deferred_zero_safe_reprime(root)", bundle)
        self.assertLess(
            bundle.index("apply_deferred_zero_history_finalize(root)"),
            bundle.index("apply_deferred_zero_safe_reprime(root)"),
            "safe re-prime must be the final Candidate A lifecycle transform",
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

        safe = ROOT / "scripts/adreno_deferred_zero_safe_reprime.py"
        if safe.exists():
            safe_text = safe.read_text(encoding="utf-8")
            self.assertNotIn("AdaptiveFrameScheduler::plan", safe_text)
            self.assertNotIn("src/adaptive_scheduler.cpp", safe_text)

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


    def test_deferred_zero_entry_preserves_fractional_anti_oscillation_without_host_drain(self) -> None:
        transform = (ROOT / "scripts/adreno_deferred_zero_history.py").read_text(encoding="utf-8")

        self.assertIn("kDeferredZeroDecisionThreshold = 6", transform)
        self.assertIn("kDeferredZeroMinimumDurationMs = 250", transform)

        decision_start = transform.index("if (adaptiveZeroGeneration)")
        decision_end = transform.index("metrics_marker =", decision_start)
        decision = transform[decision_start:decision_end]
        self.assertIn("rawSourceHistoryCount_ >= 3", decision)
        self.assertNotIn("flushPendingAndroidWork(true)", decision)

        deferred_start = transform.index("deferred-zero source-only no-framegen")
        deferred_end = transform.index("deferred-zero reprime-begin", deferred_start)
        deferred = transform[deferred_start:deferred_end]
        self.assertNotIn("waitPendingHistoryCompletionFd", deferred)
        self.assertNotIn("presentContextWithCount", deferred)

    def test_zero_history_slot_reuse_waits_on_gpu_not_present_thread(self) -> None:
        transform = (ROOT / "scripts/adreno_slot_aware_zero_history.py").read_text(encoding="utf-8")

        self.assertIn("historyCompletionWaitSemaphore", transform)
        self.assertIn("Mini::Semaphore::importFd", transform)
        self.assertIn("VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT", transform)
        self.assertIn("gameRenderSemaphores2.emplace_back", transform)
        self.assertIn("zero-history-sync-fd gpu-retire slot=", transform)

        consume_start = transform.index('new_consume = """')
        consume_end = transform.index('"""', consume_start + len('new_consume = """'))
        consume = transform[consume_start:consume_end]
        self.assertNotIn(
            "waitPendingHistoryCompletionFd(historySlot, true)",
            consume,
        )


if __name__ == "__main__":
    unittest.main()
