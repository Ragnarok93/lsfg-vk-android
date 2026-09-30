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

    def test_disabled_targeted_android_swapchain_stays_resident_and_bypasses_framegen(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        context = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        recreation_start = hooks.index("bool requiresSwapchainRecreation")
        recreation_end = hooks.index("bool supportsDeviceExtension", recreation_start)
        recreation = hooks[recreation_start:recreation_end]
        self.assertNotIn("generationActivityChanged", recreation)
        self.assertIn("const bool residentTarget = previous.targeted && next.targeted", recreation)
        self.assertIn("next.multiplier > residentCapacityMultiplier(previous)", recreation)

        self.assertNotIn("const auto createSourceOnly", hooks)
        self.assertNotIn('return createSourceOnly("generation-off")', hooks)
        self.assertIn(
            "if (activeConf.multiplier <= 1 && !activeConf.targeted)",
            hooks,
        )

        self.assertIn("enteringResidentSourceOnly", hooks)
        self.assertIn("state->context->enterSourceOnlyBypass()", hooks)
        self.assertIn("runtime stage=resident-source-only-enter", hooks)

        bypass_start = hooks.index("if (conf.targeted && conf.multiplier <= 1)")
        bypass_end = hooks.index("        try {", bypass_start)
        bypass = hooks[bypass_start:bypass_end]
        self.assertIn("Layer::ovkQueuePresentKHR(queue, pPresentInfo)", bypass)
        self.assertIn("recordSuccessfulOutputCycle(*state, *state->context", bypass)
        self.assertNotIn("state->context->present(", bypass)

        # WSI selection is intentionally generalized at swapchain setup, while
        # the protected Adreno execution island remains free of this policy.
        self.assertIn("bool residentFifoMailboxBacked = false;", hooks)
        self.assertIn("activeConf.targeted", hooks)
        self.assertIn("configuredPresentMode == VK_PRESENT_MODE_FIFO_KHR", hooks)
        self.assertIn("VK_PRESENT_MODE_MAILBOX_KHR", hooks)
        self.assertIn("resident-fifo-backend", hooks)
        adreno_start = context.index("// BEGIN ADRENO_364178AF_EXECUTION")
        adreno_end = context.index("// END ADRENO_364178AF_EXECUTION", adreno_start)
        self.assertNotIn("residentFifoMailboxBacked", context[adreno_start:adreno_end])

        self.assertIn("void enterSourceOnlyBypass();", header)
        self.assertIn("void LsContext::enterSourceOnlyBypass()", context)


if __name__ == "__main__":
    unittest.main()
