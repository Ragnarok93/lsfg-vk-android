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


    def test_disabled_targeted_android_swapchain_is_true_wsi_passthrough(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")

        create_start = hooks.index("const auto createPassThrough")
        create_end = hooks.index("#ifdef __ANDROID__", create_start + 1000)
        create_region = hooks[create_start:create_end]
        self.assertIn(
            "if (!activeConf.enable || activeConf.multiplier <= 1)",
            create_region,
        )
        self.assertNotIn(
            "activeConf.multiplier <= 1 && !activeConf.targeted",
            create_region,
        )

        recreation_start = hooks.index("bool requiresSwapchainRecreation")
        recreation_end = hooks.index("bool supportsDeviceExtension", recreation_start)
        recreation = hooks[recreation_start:recreation_end]
        self.assertIn("generationActivityChanged", recreation)
        self.assertIn("(previous.multiplier > 1) != (next.multiplier > 1)", recreation)

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
