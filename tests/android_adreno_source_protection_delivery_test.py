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

    def test_adreno_budget_reserves_serialized_handoff_without_touching_xclipse(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("SourceProtectionBudgetTracker sourceProtectionBudgetTracker_", header)
        self.assertIn("sourceProtectionBudgetTracker_.observeSource(", source)
        self.assertIn("sourceProtectionBudgetTracker_.clampTimelineBudget(", source)
        self.assertIn("sourceProtectionBudgetTracker_.observeSerializedHandoff(", source)

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

        # Predictor capacity promotion must use the same protected interval as
        # authoritative Adreno batch admission. Otherwise an LSFG-slowed source
        # can still raise the long-term Adaptive cost ceiling before admission
        # rejects the resulting work.
        hint_start = source.index("const bool safeGenerationHintValid")
        hint_end = source.index("this->adaptiveScheduler_.setSafeGenerationHint", hint_start)
        hint = source[hint_start:hint_end]
        self.assertIn("protectedCapacityIntervalMs", hint)
        self.assertIn("sourceProtectionBatchAdmission", hint)
        self.assertIn("sourceProtectionBudgetTracker_.clampTimelineBudget(", hint)
        self.assertIn("safeBatchGenerationHint(\n                maxAdaptiveGeneratedFrames, protectedCapacityIntervalMs)", hint)
        self.assertIn("safeGenerationHint(\n                maxAdaptiveGeneratedFrames, capacityIntervalMs)", hint)

        handoff_start = source.index("const auto handoffStart")
        handoff_end = source.index("if (asyncExportFailed)", handoff_start)
        handoff = source[handoff_start:handoff_end]
        self.assertIn("windowHandoffFenceWaitMs", handoff)
        self.assertIn("!this->asyncAhbHandoffEnabled_", handoff)
        self.assertIn("observeSerializedHandoff", handoff)

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

if __name__ == "__main__":
    unittest.main()
