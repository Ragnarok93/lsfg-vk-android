#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidAdrenoSourceProtectionDeliveryTest(unittest.TestCase):
    def test_admission_drop_preserves_adreno_source_history(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("conservativeAdaptiveHistoryGap", source)
        start = source.index("const bool conservativeAdmissionRejectedHistoryGap")
        end = source.index("this->lastGeneratedFrameCount_", start)
        classification = source[start:end]

        self.assertIn("generatedFrameCount == 0", classification)
        self.assertIn("!sourceHistoryWarmupActive", classification)
        self.assertIn("!sourceTimelineDiscontinuity", classification)
        self.assertIn("plannedGeneratedFrameCount > 0", classification)
        self.assertIn("conservativeAdmissionRejectedHistoryGap", classification)
        self.assertIn("!conservativeAdaptiveHistoryGap", classification)

        bypass_start = source.index("if (conservativePreCopySourceBypass)")
        bypass_end = source.index(
            "// Android path: AHardwareBuffer exchange", bypass_start
        )
        bypass = source[bypass_start:bypass_end]
        self.assertNotIn("conservativeAdaptiveHistoryGap", bypass)

        gap_start = source.index("if (conservativeAdaptiveHistoryGap)")
        gap_end = source.index("if (conservativeSourceOnlyWarmup)", gap_start)
        gap = source[gap_start:gap_end]
        self.assertIn("presentCompatibilitySourceOnly", gap)
        self.assertNotIn("requiresSourceHistoryWarmup_ = true", gap)
        self.assertNotIn("previousSourceCopySignalValid_ = false", gap)
        self.assertNotIn("presentContextWithCount", gap)

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

    def test_deferred_adreno_admission_uses_source_protection_deadline(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("sourceProtectionBatchAdmission", source)
        self.assertIn("safeBatchGenerationHint", source)
        self.assertIn("deadline_semantics=", source)
        self.assertIn('"source-protection"', source)
        self.assertIn('"synthetic-slot"', source)
        self.assertIn("compute_ready_budget_ms=", source)
        self.assertIn("presentation_slot_budget_ms=", source)

        admission_start = source.index("// Active deadline admission")
        admission_end = source.index(
            "const auto& outputCadenceForPresentation", admission_start
        )
        admission = source[admission_start:admission_end]
        self.assertIn(
            "if (sourceProtectionBatchAdmission)",
            admission,
        )
        self.assertRegex(
            admission,
            r"deadlineAdmissionPredictor_\.predict\(\s*candidate,\s*sourceBudgetMs\)",
        )
        self.assertIn(
            "sourceTimeline_.syntheticDesiredTimeNs",
            admission,
            "Xclipse/non-deferred admission must keep ideal slot semantics",
        )

    def test_xclipse_capability_async_policy_remains_separate(self) -> None:
        policy = (ROOT / "include/android_sync_policy.hpp").read_text(
            encoding="utf-8"
        )
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn('"capability-async"', policy)
        selection_start = source.index("this->asyncFramegenCompletionEnabled_ =")
        selection_end = source.index(
            "// The known-good Qualcomm/Adreno path", selection_start
        )
        selection = source[selection_start:selection_end]
        self.assertIn("!this->conservativeCrossDeviceSync_", selection)
        self.assertIn("this->syntheticQueue_ != VK_NULL_HANDLE", selection)


if __name__ == "__main__":
    unittest.main()
