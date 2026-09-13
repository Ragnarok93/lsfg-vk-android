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

    def test_adaptive_zero_generation_defers_completion_fd_without_reverse_vulkan_import(self) -> None:
        """Zero-generation completion must not import a framegen sync fd into the game VkDevice."""
        transform = "\n".join(
            (ROOT / relative).read_text(encoding="utf-8")
            for relative in (
                "scripts/adreno_syncfd_handoff.py",
                "scripts/adreno_async_zero_history.py",
                "scripts/adreno_slot_aware_zero_history.py",
            )
        )

        for marker in (
            "historyCompletionFd",
            "pendingHistoryCompletionFds_",
            "waitPendingHistoryCompletionFd",
            "adaptiveZeroGeneration",
            "zero-history-sync-fd",
            "preprocessingPending",
            "handoffFencePending",
            "presentContextWithCountAndHistoryFd",
        ):
            self.assertIn(marker, transform)

        self.assertNotIn("pendingHistoryCompletionSemaphore_", transform)
        self.assertNotIn(
            "pendingHistoryCompletionSemaphore_=Mini::Semaphore::importFd",
            transform,
        )

    def test_zero_history_retirement_is_slot_aware_without_pacing_delay(self) -> None:
        """A zero pass may overlap the next frame but must retire before its input slot is reused."""
        transform = (ROOT / "scripts/adreno_slot_aware_zero_history.py").read_text(encoding="utf-8")

        for marker in (
            "pendingHistoryCompletionFds_",
            "pendingHistoryCompletionValid_",
            "historySlot",
            "waitPendingHistoryCompletionFd(size_t historySlot",
            "pendingHistoryCompletionFds_.at(historySlot)",
            "pendingHistoryCompletionValid_.at(historySlot)",
            "zeroGenerationDirectStorage",
            "activeHistoryInput",
            "LSFG::AhbTransportMode::DirectStorage",
        ):
            self.assertIn(marker, transform)

        self.assertNotIn("int pendingHistoryCompletionFd_{-1}", transform)
        self.assertNotIn("bool pendingHistoryCompletionValid_{false}", transform)
        self.assertNotIn(
            "if (this->pendingHistoryCompletionValid_) this->waitPendingHistoryCompletionFd(true);",
            transform,
        )
        self.assertNotIn("delayUntilNextSourceOutput", transform)
        self.assertNotIn("sleep_for", transform)

    def test_zero_generation_direct_storage_owns_only_active_input(self) -> None:
        """Slot-aware retirement is safe only when zero preprocessing owns the parity input alone."""
        transform = (ROOT / "scripts/adreno_slot_aware_zero_history.py").read_text(encoding="utf-8")

        for marker in (
            "generationCount == 0 && !this->transportOnly",
            "activeHistoryInput",
            "add_external_acquire(acquireBarriers, vk, activeHistoryInput",
            "add_external_release(releaseBarriers, vk, activeHistoryInput",
        ):
            self.assertIn(marker, transform)

        self.assertIn(
            "this->asyncZeroHistoryEnabled_="
            "this->asyncAhbHandoffEnabled_ && "
            "this->asyncAhbHandoffHandleType_==VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT && "
            "ahbTransportMode==LSFG::AhbTransportMode::DirectStorage",
            transform,
        )

    def test_async_zero_history_hardens_lifecycle_and_public_api(self) -> None:
        """Deferred zero-history work must be drained on bypass/teardown without hiding waitContext."""
        hardening_path = ROOT / "scripts/adreno_async_zero_history_hardening.py"
        self.assertTrue(hardening_path.exists(), "missing async zero-history lifecycle hardening transform")
        hardening = hardening_path.read_text(encoding="utf-8")
        slot_aware = (ROOT / "scripts/adreno_slot_aware_zero_history.py").read_text(encoding="utf-8")
        apply_bundle = (ROOT / "scripts/apply-adreno-evidence-profile.py").read_text(encoding="utf-8")

        for marker in (
            "flushPendingAndroidWork",
            "performanceBackend_",
            "pendingHistoryCompletionValid_",
            "handoffFencePending",
            "enterSourceOnlyBypass",
            "presentContextWithCountAndHistoryFd",
            "preserve waitContext visibility",
            "bool waitContext(int32_t id, uint64_t timeoutNs)",
        ):
            self.assertIn(marker, hardening)

        for marker in (
            "pendingHistoryCompletionValid_.at(0)",
            "pendingHistoryCompletionValid_.at(1)",
            "waitPendingHistoryCompletionFd(historySlot, throwOnTimeout)",
            "pendingHistoryCompletionFds_.fill(-1)",
            "pendingHistoryCompletionValid_.fill(false)",
        ):
            self.assertIn(marker, slot_aware)

        self.assertIn("apply_async_zero_history_hardening(root)", apply_bundle)
        self.assertIn("apply_slot_aware_zero_history(root)", apply_bundle)
        self.assertLess(
            apply_bundle.index("apply_async_zero_history_hardening(root)"),
            apply_bundle.index("apply_slot_aware_zero_history(root)"),
        )


if __name__ == "__main__":
    unittest.main()