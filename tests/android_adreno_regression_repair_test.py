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

    def test_disabled_targeted_adreno_keeps_context_resident_and_bypasses_framegen(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        context = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        recreation_start = hooks.index("bool requiresSwapchainRecreation")
        recreation_end = hooks.index("bool supportsDeviceExtension", recreation_start)
        recreation = hooks[recreation_start:recreation_end]
        self.assertIn("generationActivationRequired", recreation)
        self.assertIn(
            "previous.multiplier <= 1 && next.multiplier > 1",
            recreation,
        )
        self.assertNotIn(
            "(previous.multiplier > 1) != (next.multiplier > 1)",
            recreation,
            "Generated -> Off must not rebuild the protected Adreno swapchain",
        )

        lookup = hooks.index("if (!state->context)")
        mutation = hooks.index("#pragma clang diagnostic push", lookup)
        source_only = hooks[lookup:mutation]
        self.assertIn("if (conf.targeted && conf.multiplier <= 1)", source_only)
        self.assertIn("state->context->enterSourceOnlyBypass()", source_only)
        self.assertIn("Layer::ovkQueuePresentKHR(queue, pPresentInfo)", source_only)
        self.assertNotIn("VK_ERROR_OUT_OF_DATE_KHR", source_only)

        self.assertIn("void enterSourceOnlyBypass();", header)
        self.assertIn("bool sourceOnlyBypassActive_{false};", header)
        self.assertIn("void LsContext::enterSourceOnlyBypass()", context)

        begin = context.index("// BEGIN ADRENO_364178AF_EXECUTION")
        end = context.index("// END ADRENO_364178AF_EXECUTION", begin)
        adreno = context[begin:end]
        self.assertNotIn(
            "sourceOnlyBypassActive_",
            adreno,
            "Off-state lifecycle repair must not alter the protected 364178af execution island",
        )


if __name__ == "__main__":
    unittest.main()
