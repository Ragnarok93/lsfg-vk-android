#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidAdrenoCompatibilityRestoreTest(unittest.TestCase):
    def test_backend_exposes_driver_id_for_policy_selection(self) -> None:
        backend = (ROOT / "framegen/public/lsfg_backend.hpp").read_text(encoding="utf-8")
        device = (ROOT / "framegen/src/core/device.cpp").read_text(encoding="utf-8")
        self.assertIn("VkDriverId driverId", backend)
        self.assertIn("driverProperties.driverID", device)

    def test_context_selects_cross_device_policy_without_global_async_rollback(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn("conservativeCrossDeviceSync_", header)
        self.assertIn("requiresConservativeCrossDeviceSync", source)
        self.assertIn("backendDiagnostics.driverId", source)
        self.assertIn("sync_policy=", source)
        self.assertIn("!this->conservativeCrossDeviceSync_", source)
        self.assertIn("this->asyncAhbHandoffEnabled_", source)
        self.assertIn("this->asyncFramegenCompletionEnabled_", source)

    def test_conservative_warmup_is_source_only_but_fractional_gaps_keep_history(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn("conservativeSourceOnlyWarmup", source)
        self.assertIn("conservativeFractionalHistoryGap", source)
        self.assertIn("conservativeTrueSourceOnlyCycle", source)
        helper = source.split("const auto presentCompatibilitySourceOnly", 1)[1].split(
            "if (conservativeSourceOnlyWarmup)", 1
        )[0]
        warmup = source.split("if (conservativeSourceOnlyWarmup)", 1)[1].split(
            "if (historyOnly)", 1
        )[0]
        self.assertIn("Layer::ovkQueuePresentKHR", helper)
        self.assertIn("presentCompatibilitySourceOnly", warmup)
        self.assertNotIn("presentContextWithCount", warmup)
        self.assertNotIn("presentContextWithCountExportSyncFd", warmup)

    def test_adreno_uses_parity_safe_source_reprime_without_startup_delay(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn("kConservativeSourceReprimeFrames = 2", source)
        self.assertIn("kConservativeSourceReprimeFrames - 1", source)
        self.assertIn(
            "this->conservativeCrossDeviceSync_ ? kConservativeSourceReprimeFrames",
            source,
        )
        selection = source.split(
            "this->conservativeCrossDeviceSync_ =", 1
        )[1].split("std::cerr << \"lsfg-vk: Android AHB context created", 1)[0]
        self.assertIn("sourceHistoryWarmupRemaining_ = 0", selection)
        self.assertIn("requiresSourceHistoryWarmup_ = false", selection)

    def test_non_async_history_preprocessing_waits_before_ahb_reuse(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        history = source.split("if (historyOnly)", 1)[1].split(
            "// 2. Tell framegen", 1
        )[0]
        fallback = history.split("} else {", 1)[1]
        self.assertIn("historyRequiresHostCompletionWait = true", fallback)
        self.assertIn("waitContext", history)

    def test_adreno_generated_cycles_use_sync_fd_input_and_completion(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        selection = source.split(
            "this->conservativeCrossDeviceSync_ =", 1
        )[1].split("std::cerr << \"lsfg-vk: Android AHB context created", 1)[0]
        self.assertIn("syncFdHandoffSupported", selection)
        self.assertIn(
            "this->conservativeCrossDeviceSync_\n"
            "            ? syncFdHandoffSupported",
            selection,
        )
        self.assertIn(
            "? VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT",
            selection,
        )
        self.assertIn(
            "this->asyncFramegenCompletionEnabled_ =\n"
            "        syncFdHandoffSupported && gameImportSemaphoreFd != nullptr",
            selection,
        )
        self.assertNotIn(
            "this->asyncFramegenCompletionEnabled_ =\n"
            "        !this->conservativeCrossDeviceSync_",
            selection,
        )

        handoff = source.split(
            "bool useAsyncHandoff =", 1
        )[1].split("bool asyncSubmissionIssued", 1)[0]
        self.assertIn("!conservativeSourceOnlyWarmup", handoff)
        self.assertIn("!conservativeFractionalHistoryGap", handoff)
        self.assertIn("!conservativeTrueSourceOnlyCycle", handoff)

    def test_fixed_generated_frames_are_not_dropped_by_adaptive_deadline_policy(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        generated_start = source.index(
            "// 4. Generated presentation is opportunistic."
        )
        source_start = source.index(
            "// 5. Present the real game frame", generated_start
        )
        generated = source[generated_start:source_start]

        deadline = generated.split(
            "const uint64_t syntheticAdmissionNowNs = monotonicNowNs();", 1
        )[1].split("pass.acquireSemaphores.at(i)", 1)[0]
        self.assertIn("enforcePostDispatchSyntheticDeadline", deadline)
        self.assertIn(
            "conf.adaptiveFramegen && !this->conservativeCrossDeviceSync_",
            deadline,
        )

    def test_adreno_adaptive_commits_pre_admitted_work_after_host_completion(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        generated_start = source.index(
            "// 4. Generated presentation is opportunistic."
        )
        source_start = source.index(
            "// 5. Present the real game frame", generated_start
        )
        generated = source[generated_start:source_start]

        self.assertIn("enforcePostDispatchSyntheticDeadline", generated)
        self.assertIn(
            "conf.adaptiveFramegen && !this->conservativeCrossDeviceSync_",
            generated,
        )
        deadline = generated.split(
            "const uint64_t syntheticAdmissionNowNs = monotonicNowNs();", 1
        )[1].split("pass.acquireSemaphores.at(i)", 1)[0]
        self.assertIn("if (enforcePostDispatchSyntheticDeadline", deadline)

    def test_adreno_fixed_mode_uses_bounded_generated_acquire_without_changing_xclipse(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        generated_start = source.index(
            "// 4. Generated presentation is opportunistic."
        )
        source_start = source.index(
            "// 5. Present the real game frame", generated_start
        )
        generated = source[generated_start:source_start]

        self.assertIn("generatedAcquireTimeoutNs", generated)
        self.assertIn(
            "!conf.adaptiveFramegen && this->conservativeCrossDeviceSync_",
            generated,
        )
        self.assertIn("runtimeWaitTimeoutNs()", generated)
        self.assertIn(": 0;", generated)

    def test_adreno_opaque_export_failure_uses_adreno_reprime_not_xclipse_warmup(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        failure = source.split("if (asyncExportFailed)", 1)[1].split(
            "const VkSemaphore sourceReady", 1
        )[0]
        self.assertIn("this->conservativeCrossDeviceSync_", failure)
        self.assertIn("kConservativeSourceReprimeFrames", failure)
        self.assertIn("kSourceHistoryWarmupFrames", failure)

    def test_adreno_true_source_only_cycle_bypasses_private_ahb_copy(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        bypass_start = source.index("if (conservativePreCopySourceBypass)")
        private_copy = source.index(
            "pass.preCopySemaphores.at(0) = Mini::Semaphore", bypass_start
        )
        bypass = source[bypass_start:private_copy]

        self.assertIn("Layer::ovkQueuePresentKHR", bypass)
        self.assertIn("previousSourceCopySignalValid_ = false", bypass)
        self.assertIn("kConservativeSourceReprimeFrames - 1", bypass)
        self.assertNotIn("copySwapchainToExternalAhb", bypass)
        self.assertNotIn("submitAndWaitForAhbHandoff", bypass)

    def test_adreno_reprime_copy_is_queued_without_host_fence_wait(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        handoff_start = source.index("const auto handoffStart")
        host_fallback = source.index(
            "submitAndWaitForAhbHandoff", handoff_start
        )
        handoff = source[handoff_start:host_fallback]

        self.assertIn("conservativeSourceOnlyWarmup", handoff)
        self.assertIn("submitAhbHandoff", handoff)
        self.assertIn("queuedCopyWithoutHostWait", handoff)

    def test_adreno_fractional_gap_queues_copy_without_zero_count_framegen(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        handoff_start = source.index("const auto handoffStart")
        history_start = source.index("if (historyOnly)", handoff_start)
        pre_history = source[handoff_start:history_start]

        self.assertIn("conservativeFractionalHistoryGap", pre_history)
        self.assertIn("queuedCopyWithoutHostWait", pre_history)
        self.assertIn(
            'presentCompatibilitySourceOnly(\n'
            '            "compat-fractional-history-copy"',
            pre_history,
        )
        fractional = pre_history.split(
            "if (conservativeFractionalHistoryGap)", 1
        )[1]
        self.assertNotIn("presentContextWithCount(", fractional)
        self.assertNotIn("presentContextWithCountExportSyncFd(", fractional)

    def test_handoff_metrics_separate_submit_wait_and_cross_frame_dependencies(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        for token in (
            "windowHandoffSubmitMs",
            "windowHandoffFenceWaitMs",
            "windowHandoffPrevSourceDeps",
            "windowHandoffBatchDeps",
        ):
            self.assertIn(token, header)
        for token in (
            "ahb_submit_avg_ms=",
            "ahb_host_wait_avg_ms=",
            "ahb_prev_source_deps=",
            "ahb_batch_deps=",
        ):
            self.assertIn(token, source)

    def test_adreno_async_completion_does_not_reopen_zero_count_framegen(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        fractional = source.index("if (conservativeFractionalHistoryGap)")
        warmup = source.index("if (conservativeSourceOnlyWarmup)", fractional)
        history = source.index("if (historyOnly)", warmup)

        self.assertLess(fractional, history)
        self.assertLess(warmup, history)
        pre_history = source[fractional:history]
        self.assertIn("presentCompatibilitySourceOnly", pre_history)
        self.assertNotIn("presentContextWithCountExportSyncFd", pre_history)
        self.assertNotIn("presentContextWithCount(", pre_history)

    def test_generated_compatibility_path_retains_bounded_completion_wait(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        generated = source.split("// 2. Tell framegen", 1)[1]
        self.assertIn(
            "bool requireHostCompletionWait = !this->asyncFramegenCompletionEnabled_",
            generated,
        )
        self.assertIn("runtimeWaitTimeoutNs()", generated)
        self.assertIn("waitContext", generated)

    def test_runtime_policy_label_matches_split_adreno_topology(self) -> None:
        policy = (ROOT / "include/android_sync_policy.hpp").read_text(encoding="utf-8")
        self.assertIn("syncfd-generated-async-adreno", policy)
        self.assertIn("capability-async", policy)

    def test_xclipse_async_path_is_not_removed(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn("syncFdHandoffSupported", source)
        self.assertIn("presentContextWithCountExportSyncFd", source)
        self.assertIn("VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT", source)


if __name__ == "__main__":
    unittest.main()
