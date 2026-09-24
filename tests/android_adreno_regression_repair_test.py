#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidAdrenoRegressionRepairTest(unittest.TestCase):
    def test_adreno_source_handoff_restores_historical_opaque_fd_attempt(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        start = source.index("const bool syncFdHandoffSupported")
        end = source.index("const bool xclipseCompatibilityPath", start)
        selection = source[start:end]

        # Protected Adreno must not substitute SYNC_FD for the September 18
        # source handoff. Turnip can report OPAQUE_FD feature bits as zero even
        # though the real OPAQUE_FD create/export/import transaction succeeds,
        # so the actual transaction remains the runtime probe and failure falls
        # back to the bounded host fence.
        self.assertIn("adrenoHistoricalOpaqueAttempt", selection)
        self.assertIn("info.androidSyncFdSemaphoreSupported", selection)
        self.assertIn("backendDiagnostics.externalSemaphoreSyncFd", selection)
        self.assertIn(
            "? (opaqueFdHandoffSupported || adrenoHistoricalOpaqueAttempt)",
            selection,
        )
        self.assertIn(
            "this->asyncAhbHandoffHandleType_ =\n"
            "            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;",
            selection,
        )
        self.assertIn(
            ": (syncFdHandoffSupported || opaqueFdHandoffSupported)",
            selection,
            "Generic/Xclipse routing must remain capability-driven.",
        )

    def test_disabled_targeted_android_swapchain_stays_resident_and_bypasses_framegen(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        context = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        create_start = hooks.index("const auto createPassThrough")
        create_end = hooks.index("#ifdef __ANDROID__", create_start + 1000)
        create_region = hooks[create_start:create_end]
        self.assertIn(
            "activeConf.multiplier <= 1 && !activeConf.targeted",
            create_region,
        )
        self.assertNotIn(
            "if (!activeConf.enable || activeConf.multiplier <= 1)",
            create_region,
        )

        recreation_start = hooks.index("bool requiresSwapchainRecreation")
        recreation_end = hooks.index("bool supportsDeviceExtension", recreation_start)
        recreation = hooks[recreation_start:recreation_end]
        self.assertNotIn("generationActivityChanged", recreation)
        self.assertNotIn("(previous.multiplier > 1) != (next.multiplier > 1)", recreation)

        present_start = hooks.index("if (conf.targeted && conf.multiplier <= 1)")
        present_end = hooks.index("try {", present_start)
        present = hooks[present_start:present_end]
        self.assertIn("state->context->enterSourceOnlyBypass()", present)
        self.assertIn("Layer::ovkQueuePresentKHR(queue, pPresentInfo)", present)
        self.assertIn("void enterSourceOnlyBypass();", header)
        self.assertIn("void LsContext::enterSourceOnlyBypass()", context)

    def test_adaptive_adreno_requires_clean_source_baseline_before_admission(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        admission_start = source.index("// Active deadline admission")
        admission_end = source.index(
            "const auto& outputCadenceForPresentation", admission_start
        )
        admission = source[admission_start:admission_end]

        self.assertIn("sourceProtectionBaselineValid", admission)
        self.assertIn(
            "sourceProtectionBatchAdmission && !sourceProtectionBaselineValid",
            admission,
        )
        self.assertIn(
            "generatedFrameCount = 0;",
            admission,
            "Adaptive Adreno must collect a clean source-only interval before "
            "synthetic work can consume the source timeline.",
        )
        self.assertIn(
            "sourceProtectionBaselineValid && sourceProtectionBatchAdmission",
            admission,
            "Protected Adreno may only clamp/admit against a source budget after "
            "clean source-only cadence established the baseline.",
        )

    def test_adreno_prior_compute_overload_escapes_before_source_copy(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        begin = source.index("// BEGIN ADRENO_364178AF_EXECUTION")
        end = source.index("// END ADRENO_364178AF_EXECUTION", begin)
        adreno = source[begin:end]

        prefix = source[:begin]
        self.assertIn("protectedAdrenoPriorComputeOverBudget", prefix)
        self.assertIn("adaptiveFlowRetainedTotalLsfgMs_", prefix)
        self.assertIn("lastSourceCadenceObservation_", prefix)
        escape = adreno.index("protectedAdrenoPriorComputeOverBudget")
        first_copy = adreno.index("copySwapchainToExternalAhb", escape)
        self.assertLess(escape, first_copy)
        escape_block = adreno[escape:first_copy]
        self.assertIn("SourceCadenceObservation::SourceOnly", escape_block)
        self.assertIn("sourceHistoryWarmupRemaining_ = 1", escape_block)
        self.assertIn("game-render-overload-bypass", escape_block)
        self.assertNotIn("submitAndWaitForAhbHandoff(", escape_block)

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
