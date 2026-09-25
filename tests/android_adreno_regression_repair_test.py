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


if __name__ == "__main__":
    unittest.main()
