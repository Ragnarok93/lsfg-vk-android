#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidAdrenoRegressionRepairTest(unittest.TestCase):
    def test_adreno_source_handoff_respects_reported_fd_capabilities(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        start = source.index("const bool syncFdHandoffSupported")
        end = source.index("const bool xclipseCompatibilityPath", start)
        selection = source[start:end]

        # The current GameNative/Turnip wrapper reports OPAQUE_FD unsupported
        # and SYNC_FD export/import supported. Protected Adreno must therefore
        # select the capability-advertised SYNC_FD source handoff instead of
        # probing a rejected OPAQUE_FD transaction and serializing on a host fence.
        self.assertNotIn("adrenoHistoricalOpaqueAttempt", selection)
        self.assertIn("syncFdHandoffSupported", selection)
        self.assertIn("opaqueFdHandoffSupported", selection)
        self.assertIn(
            "(syncFdHandoffSupported || opaqueFdHandoffSupported)",
            selection,
        )
        self.assertIn(
            "syncFdHandoffSupported\n"
            "            ? VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT",
            selection,
        )
        self.assertIn(
            ": VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT",
            selection,
        )

    def test_disabled_targeted_android_swapchain_releases_framegen_context(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        context = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        recreation_start = hooks.index("bool requiresSwapchainRecreation")
        recreation_end = hooks.index("bool supportsDeviceExtension", recreation_start)
        recreation = hooks[recreation_start:recreation_end]
        self.assertIn("generationActivityChanged", recreation)
        self.assertIn(
            "(previous.multiplier > 1) != (next.multiplier > 1)",
            recreation,
        )

        source_only_start = hooks.index("const auto createSourceOnly")
        source_only_end = hooks.index("#ifdef __ANDROID__", source_only_start)
        source_only = hooks[source_only_start:source_only_end]
        self.assertIn("VkSwapchainCreateInfoKHR sourceOnlyCreateInfo = *pCreateInfo", source_only)
        self.assertIn("choosePresentMode(", source_only)
        self.assertNotIn("LsContext", source_only)
        self.assertNotIn("requiredTransferUsage", source_only)
        self.assertNotIn("residentCapacityMultiplier", source_only)

        self.assertIn(
            "if (activeConf.targeted && activeConf.multiplier <= 1)",
            hooks,
        )
        self.assertIn('return createSourceOnly("generation-off")', hooks)
        self.assertNotIn("state->context->enterSourceOnlyBypass()", hooks)

        # The lifecycle helper remains valid for true context reset paths; Off
        # simply no longer invokes it every frame on a resident LSFG swapchain.
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
        self.assertIn("sourceHistoryWarmupRemaining_ = 0", escape_block)
        self.assertIn("requiresSourceHistoryWarmup_ = false", escape_block)
        self.assertIn("AdrenoSourceProtectionBackoffReason::ComputeOverBudget", escape_block)
        self.assertIn("reprime=deferred", escape_block)
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
        self.assertIn("sourceHistoryWarmupRemaining_ = 0", escape_block)
        self.assertIn("requiresSourceHistoryWarmup_ = false", escape_block)
        self.assertIn("AdrenoSourceProtectionBackoffReason::AdmissionRejected", escape_block)
        self.assertIn("reprime=deferred", escape_block)
        self.assertIn("game-render-admission-bypass", escape_block)
        self.assertNotIn("presentContextWithCount(", escape_block)
        self.assertNotIn("submitAndWaitForAhbHandoff(", escape_block)

    def test_adreno_protection_bypasses_do_not_masquerade_as_source_pair_corruption(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        for reason in (
            "AdmissionBypass",
            "OverloadBypass",
            "RetirementBackpressure",
            "FractionalGap",
        ):
            self.assertIn(reason, header)
            self.assertIn(reason, source)

        begin = source.index("// BEGIN ADRENO_364178AF_EXECUTION")
        end = source.index("// END ADRENO_364178AF_EXECUTION", begin)
        adreno = source[begin:end]

        overload = adreno[
            adreno.index("if (protectedAdrenoPriorComputeOverBudget)"):
            adreno.index("if (conservativeAdmissionRejectedHistoryGap)")
        ]
        self.assertIn(
            "lastHistoryInvalidationReason_ =\n"
            "                SourceHistoryInvalidationReason::None",
            overload,
        )
        self.assertIn(
            "SourceHistoryInvalidationReason::OverloadBypass",
            overload,
        )
        self.assertNotIn(
            "SourceHistoryInvalidationReason::SourcePairMismatch",
            overload,
        )

        admission_start = adreno.index("if (conservativeAdmissionRejectedHistoryGap)")
        admission_end = adreno.index("pass.preCopySemaphores.at(0)", admission_start)
        admission = adreno[admission_start:admission_end]
        self.assertIn(
            "lastHistoryInvalidationReason_ =\n"
            "                SourceHistoryInvalidationReason::None",
            admission,
        )
        self.assertIn(
            "SourceHistoryInvalidationReason::AdmissionBypass",
            admission,
        )
        self.assertNotIn(
            "SourceHistoryInvalidationReason::SourcePairMismatch",
            admission,
        )

        # Warmup/preprocess work is not clean native source-only evidence.
        warmup_start = adreno.index("if (sourceHistoryWarmupActive)")
        warmup_end = adreno.index("this->lastDispatchedGeneratedFrameCount_ = generatedFrameCount", warmup_start)
        warmup = adreno[warmup_start:warmup_end]
        self.assertIn(
            "SourceCadenceObservation::HistoryMaintenance",
            warmup,
        )
        self.assertNotIn(
            "SourceCadenceObservation::SourceOnly",
            warmup,
        )


if __name__ == "__main__":
    unittest.main()
