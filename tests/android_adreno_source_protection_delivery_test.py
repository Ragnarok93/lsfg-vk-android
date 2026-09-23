#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidAdrenoSourceProtectionDeliveryTest(unittest.TestCase):
    def test_admission_drop_preserves_adreno_source_history(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("conservativeHistoryGap", source)
        self.assertIn("conservativeFixedHistoryGap", source)
        start = source.index("const bool conservativeAdmissionRejectedHistoryGap")
        end = source.index("this->lastGeneratedFrameCount_", start)
        classification = source[start:end]

        self.assertIn("generatedFrameCount == 0", classification)
        self.assertIn("!sourceHistoryWarmupActive", classification)
        self.assertIn("!sourceTimelineDiscontinuity", classification)
        self.assertIn("plannedGeneratedFrameCount > 0", classification)
        self.assertIn("conservativeAdmissionRejectedHistoryGap", classification)
        self.assertIn("conservativeAdaptiveHistoryGap", classification)

        bypass_start = source.index("if (conservativePreCopySourceBypass)")
        bypass_end = source.index(
            "// Android path: AHardwareBuffer exchange", bypass_start
        )
        bypass = source[bypass_start:bypass_end]
        self.assertNotIn("conservativeHistoryGap", bypass)

        history_start = source.index("if (historyOnly)")
        generation_start = source.index(
            "// 2. Tell framegen to generate intermediary frames.", history_start
        )
        history = source[history_start:generation_start]
        self.assertIn("presentContextWithCount(", history)
        self.assertIn("historyRequiresHostCompletionWait", history)
        self.assertNotIn("compat-adaptive-history-copy", source)

    def test_history_invalidation_has_explicit_reason(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("SourceHistoryInvalidationReason", header)
        for reason in (
            "Startup",
            "TimelineDiscontinuity",
            "SyncExportFailure",
            "SyncImportFailure",
            "ContextRecreate",
            "SourcePairMismatch",
            "AbandonedBatch",
            "TrueOwnershipFailure",
        ):
            self.assertIn(reason, header)

        self.assertIn("sourceHistoryInvalidationReasonName", source)
        self.assertIn("history_invalidation_reason=", source)
        self.assertIn("history_reprime_reason=", source)

    def test_generated_metrics_distinguish_wsi_acceptance_from_display(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        for metric in (
            "windowGeneratedDispatched",
            "windowGeneratedCompleted",
            "windowGeneratedCopySubmitted",
            "windowGeneratedWsiSubmitted",
            "windowGeneratedWsiAccepted",
            "windowGeneratedDisplayConfirmed",
            "windowGeneratedDisplayUnknown",
        ):
            self.assertIn(metric, header)

        self.assertIn("generated_dispatched=", source)
        self.assertIn("generated_completed=", source)
        self.assertIn("generated_copy_submitted=", source)
        self.assertIn("generated_wsi_submitted=", source)
        self.assertIn("generated_wsi_accepted=", source)
        self.assertIn("generated_display_confirmed=", source)
        self.assertIn("generated_display_unknown=", source)
        self.assertIn("generated_delivery_confidence=", source)
        self.assertIn("wsi-accepted-only", source)

    def test_adreno_admission_uses_source_protection_deadline(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("sourceProtectionBatchAdmission", source)
        self.assertIn("safeBatchGenerationHint", source)
        self.assertIn("deadline_semantics=", source)
        self.assertIn('"source-protection"', source)
        self.assertIn('"synthetic-slot"', source)
        self.assertIn("compute_ready_budget_ms=", source)
        self.assertIn("presentation_slot_budget_ms=", source)

        policy_start = source.index("const bool sourceProtectionBatchAdmission")
        policy_end = source.index("double computeReadyBudgetMs", policy_start)
        policy = source[policy_start:policy_end]
        self.assertIn("this->conservativeCrossDeviceSync_", policy)
        self.assertNotIn("deferredAdrenoCompletionEnabled_", policy)

        admission_start = source.index("// Active deadline admission")
        admission_end = source.index(
            "const auto& outputCadenceForPresentation", admission_start
        )
        admission = source[admission_start:admission_end]
        self.assertIn(
            "if ((conf.adaptiveFramegen || sourceProtectionBatchAdmission)",
            admission,
        )
        self.assertIn("sourceBudgetMs", admission)
        self.assertIn(
            "if (sourceProtectionBatchAdmission)",
            admission,
        )
        self.assertIn(
            "deadlineAdmissionPredictor_.predict(\n"
            "                                    candidate, sourceBudgetMs)",
            admission,
        )
        self.assertIn(
            "sourceTimeline_.syntheticDesiredTimeNs",
            admission,
            "Xclipse/non-deferred admission must keep ideal slot semantics",
        )

    def test_adreno_budget_excludes_host_fence_wall_wait_without_touching_xclipse(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        scheduler = (ROOT / "include/adaptive_scheduler.hpp").read_text(encoding="utf-8")

        self.assertIn("SourceProtectionBudgetTracker sourceProtectionBudgetTracker_", header)
        self.assertIn("sourceProtectionBudgetTracker_.observeSource(", source)
        self.assertIn("sourceProtectionBudgetTracker_.clampTimelineBudget(", source)
        self.assertIn("observeSerializedCopyCost", scheduler)
        self.assertNotIn("observeSerializedHandoff", scheduler)
        self.assertNotIn("sourceProtectionBudgetTracker_.observeSerialized", source)

        admission_start = source.index("// Active deadline admission")
        admission_end = source.index(
            "const auto& outputCadenceForPresentation", admission_start
        )
        admission = source[admission_start:admission_end]
        self.assertIn("rawSourceBudgetMs", admission)
        self.assertIn(
            "sourceProtectionBatchAdmission\n"
            "                        ? this->sourceProtectionBudgetTracker_.clampTimelineBudget(",
            admission,
        )
        self.assertIn(": rawSourceBudgetMs", admission)

        # Predictor capacity promotion uses the protected interval on Adreno,
        # while generic/Xclipse keeps its original raw capacity interval.
        hint_start = source.index("const double protectedCapacityIntervalMs")
        hint_end = source.index("this->adaptiveScheduler_.setSafeGenerationHint", hint_start)
        hint = source[hint_start:hint_end]
        self.assertIn("sourceProtectionBatchAdmission", hint)
        self.assertIn("sourceProtectionBudgetTracker_.clampTimelineBudget(", hint)
        self.assertIn("safeBatchGenerationHint(", hint)
        self.assertIn("safeGenerationHint(", hint)
        self.assertIn("capacityIntervalMs", hint)

        handoff_start = source.index("const auto handoffStart")
        handoff_end = source.index("if (asyncExportFailed)", handoff_start)
        handoff = source[handoff_start:handoff_end]
        self.assertIn("windowHandoffFenceWaitMs", handoff)
        self.assertNotIn("sourceProtectionBudgetTracker_.observeSerialized", handoff)

    def test_adreno_budget_telemetry_reports_effective_policy(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        policy = (ROOT / "include/android_sync_policy.hpp").read_text(encoding="utf-8")

        for field in (
            "source_protected_interval_ms=",
            "source_budget_raw_ms=",
            "source_budget_effective_ms=",
            "source_budget_copy_reserve_ms=",
            "source_budget_observation=",
            "source_protection_baseline_valid=",
            "source_protection_copy_cost_valid=",
        ):
            self.assertIn(field, source)

        self.assertIn('"adreno-source-protected-host-completion"', policy)
        self.assertNotIn('"opaque-fd-input-host-completion-adreno"', policy)

    def test_xclipse_capability_async_policy_remains_separate(self) -> None:
        policy = (ROOT / "include/android_sync_policy.hpp").read_text(
            encoding="utf-8"
        )
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn('"capability-async"', policy)
        selection_start = source.index("this->asyncFramegenCompletionEnabled_ =")
        selection_end = source.index(
            "const bool xclipseCompatibilityPath", selection_start
        )
        selection = source[selection_start:selection_end]
        self.assertIn("!this->conservativeCrossDeviceSync_", selection)
        self.assertIn("syncFdHandoffSupported", selection)
        self.assertIn("gameImportSemaphoreFd != nullptr", selection)

    def test_adreno_zero_history_exports_release_without_deferring_generated_output(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("asyncHistoryCompletionEnabled_", header)
        history_start = source.index("if (historyOnly)")
        generation_start = source.index(
            "// 2. Tell framegen to generate intermediary frames.", history_start
        )
        history = source[history_start:generation_start]

        self.assertIn("presentContextWithCountExportSyncFd", history)
        self.assertIn("conservativePendingHistoryCompleteSemaphore_", header)
        self.assertIn("conservativePendingHistoryCompleteValid_", header)
        self.assertIn("conservativePendingHistoryCompleteValid_", source)
        self.assertIn("VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT", history)
        self.assertIn("historyRequiresHostCompletionWait = false", history)

        # Generated Adreno completion remains the proven bounded host wait.
        selection_start = source.index("this->asyncFramegenCompletionEnabled_ =")
        selection_end = source.index(
            "// Rejected Adreno experiment", selection_start
        )
        generated_completion_gate = source[selection_start:selection_end]
        self.assertIn(
            "!this->conservativeCrossDeviceSync_", generated_completion_gate
        )
        self.assertNotIn(
            "asyncHistoryCompletionEnabled_", generated_completion_gate
        )

    def test_adreno_history_release_survives_source_only_bypass_until_reuse(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("conservativePendingHistoryCompleteSemaphore_", header)
        self.assertIn("conservativePendingHistoryCompleteValid_", header)

        bypass_start = source.index("if (conservativePreCopySourceBypass)")
        bypass_end = source.index("// Android path: AHardwareBuffer exchange", bypass_start)
        bypass = source[bypass_start:bypass_end]
        self.assertNotIn(
            "conservativePendingHistoryCompleteValid_ = false",
            bypass,
            "Source-only bypass must not discard a private history release that still guards AHB reuse",
        )

        copy_start = source.index(
            "std::vector<VkSemaphore> gameRenderSemaphores2 = gameRenderSemaphores;"
        )
        handoff_start = source.index("const auto handoffStart", copy_start)
        copy_dependencies = source[copy_start:handoff_start]
        self.assertIn("conservativePendingHistoryCompleteValid_", copy_dependencies)
        self.assertIn("conservativePendingHistoryCompleteSemaphore_", copy_dependencies)

        submit_start = source.index("if (useAsyncHandoff)", handoff_start)
        submit_end = source.index("if (consumeDeferredAdrenoBatchComplete)", submit_start)
        submit = source[submit_start:submit_end]
        self.assertIn(
            "conservativePendingHistoryCompleteValid_ = false",
            submit,
            "The carried release may be cleared only after a source-copy submit has consumed it",
        )

    def test_adreno_source_budget_does_not_charge_full_host_fence_wall_wait(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        scheduler = (ROOT / "src/adaptive_scheduler.cpp").read_text(encoding="utf-8")
        header = (ROOT / "include/adaptive_scheduler.hpp").read_text(encoding="utf-8")

        self.assertIn("observeSerializedCopyCost", header)
        self.assertIn("serializedCopyReserveMs", header)
        self.assertNotIn("observeSerializedHandoff", header)
        self.assertNotIn("observeSerializedHandoff", scheduler)

        handoff_start = source.index("if (!useAsyncHandoff && !asyncSubmissionIssued)")
        handoff_end = source.index("metrics.windowSyncHandoffs++", handoff_start)
        handoff = source[handoff_start:handoff_end]
        self.assertNotIn(
            "sourceProtectionBudgetTracker_.observeSerialized",
            handoff,
            "Fence wall time includes game-render dependencies and must remain diagnostics only",
        )

    def test_adreno_diagnostics_separate_history_and_handoff_costs(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        for token in (
            "windowHistoryPreprocessSubmitMs",
            "windowHistoryPreprocessHostWaitMs",
            "windowHistoryAsyncReleases",
            "windowHistoryHostCompletions",
        ):
            self.assertIn(token, header)

        for token in (
            "history_preprocess_submit_avg_ms=",
            "history_preprocess_host_wait_avg_ms=",
            "history_async_releases=",
            "history_host_completions=",
            "source_budget_raw_ms=",
            "source_budget_effective_ms=",
            "source_budget_copy_reserve_ms=",
            "source_budget_observation=",
        ):
            self.assertIn(token, source)

    def test_adreno_adaptive_flow_fallback_budget_uses_protected_source_budget(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        budget_start = source.index("const double adaptiveFlowBatchBudgetMs =")
        budget_end = source.index("const auto nextAdaptiveFlowBatch", budget_start)
        budget = source[budget_start:budget_end]

        self.assertIn("this->conservativeCrossDeviceSync_", budget)
        self.assertIn(
            "this->sourceProtectionBudgetTracker_.clampTimelineBudget",
            budget,
            "Adreno Flow fallback budget must not expand with an LSFG-stretched source interval",
        )
        self.assertIn("protectedAdrenoTargetBudgetMs", budget)

        # The clamp is guarded by the Adreno compatibility selector, so the
        # Xclipse/generic budget route remains unchanged.
        clamp_pos = budget.index(
            "this->sourceProtectionBudgetTracker_.clampTimelineBudget"
        )
        guard = budget[max(0, clamp_pos - 500):clamp_pos]
        self.assertIn("this->conservativeCrossDeviceSync_", guard)

    def test_adreno_cadence_evidence_distinguishes_copy_only_warmup_from_history(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        warmup_start = source.index("if (conservativeSourceOnlyWarmup)")
        warmup_end = source.index("if (historyOnly)", warmup_start)
        warmup = source[warmup_start:warmup_end]
        self.assertIn(
            "SourceCadenceObservation::SourceOnly",
            warmup,
            "Copy-only Adreno warmup is the cleanest available source baseline",
        )
        self.assertNotIn("SourceCadenceObservation::HistoryMaintenance", warmup)

        history_start = source.index("if (historyOnly)", warmup_end)
        history_end = source.index(
            "// 2. Tell framegen to generate intermediary frames.", history_start
        )
        history = source[history_start:history_end]
        self.assertIn("SourceCadenceObservation::HistoryMaintenance", history)

        generated = source[history_end:]
        self.assertIn(
            "this->lastSourceCadenceObservation_ =\n"
            "        SourceCadenceObservation::Generated;",
            generated,
        )

    def test_xclipse_non_async_history_fallback_keeps_bounded_wait(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        history_start = source.index("if (historyOnly)")
        history_end = source.index(
            "// 2. Tell framegen to generate intermediary frames.", history_start
        )
        history = source[history_start:history_end]
        non_export_start = history.index("} else {", history.index("if (exportHistoryRelease)"))
        non_export = history[non_export_start:]
        self.assertIn("!this->conservativeCrossDeviceSync_", non_export)
        self.assertIn("historyRequiresHostCompletionWait = true", non_export)
        self.assertIn("waitContext", history)

    def test_xclipse_history_completion_path_keeps_existing_capability_async_behavior(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        constructor_start = source.index("this->asyncFramegenCompletionEnabled_ =")
        constructor_end = source.index("// Match the device-proven baseline", constructor_start)
        selection = source[constructor_start:constructor_end]

        self.assertIn("!this->conservativeCrossDeviceSync_", selection)
        self.assertIn("syncFdHandoffSupported", selection)
        self.assertIn("gameImportSemaphoreFd != nullptr", selection)


if __name__ == "__main__":
    unittest.main()
