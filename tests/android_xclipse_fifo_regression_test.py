#!/usr/bin/env python3
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class AndroidXclipseFifoRegressionTest(unittest.TestCase):
    def test_generic_xclipse_acquire_retirements_wait_for_pass_fence(self) -> None:
        """Previous present waits stay alive until the acquire wait has retired."""
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("acquiredPresentWaitRetentions", header)
        self.assertIn("presentAcquireRetirementsArmed", header)

        generated_start = source.index(
            "// 4. Generated presentation is opportunistic."
        )
        generated_end = source.index(
            "// 5. Present the real game frame", generated_start
        )
        generated = source[generated_start:generated_end]

        acquire = generated.index("Layer::ovkAcquireNextImageKHR")
        submit = generated.index("postCopyBuf.submit(", acquire)
        acquire_to_submit = generated[acquire:submit]

        self.assertNotIn(
            "releasePresentWaitRetirements(imageIdx)",
            acquire_to_submit,
            "Xclipse must not destroy prior present waits before the acquire wait is submitted",
        )
        self.assertIn("acquiredPresentWaitRetentions", acquire_to_submit)
        self.assertIn("std::move", acquire_to_submit)

        recycle_start = source.index("bool LsContext::tryRecyclePass")
        recycle_end = source.index(
            "bool LsContext::submitPassCompletionFence", recycle_start
        )
        recycle = source[recycle_start:recycle_end]
        self.assertIn("presentAcquireRetirementsArmed", recycle)
        self.assertIn("acquiredPresentWaitRetentions.clear()", recycle)

        arm_start = source.index(
            "const auto armPassGpuRetirement =",
        )
        arm_end = source.index(
            "if (this->conservativeCrossDeviceSync_",
            arm_start,
        )
        arm = source[arm_start:arm_end]
        self.assertIn("presentAcquireRetirementsArmed", arm)

    def test_retirement_repair_stays_out_of_the_protected_adreno_island(self) -> None:
        """Adreno keeps the September-18 compatibility path unchanged."""
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        begin = source.index("// BEGIN ADRENO_364178AF_EXECUTION")
        end = source.index("// END ADRENO_364178AF_EXECUTION", begin)
        adreno = source[begin:end]

        self.assertNotIn("acquiredPresentWaitRetentions", adreno)
        self.assertNotIn("presentAcquireRetirementsArmed", adreno)
        self.assertIn("runtimeWaitTimeoutNs()", adreno)
        self.assertIn("generated-before-source", source)

    def test_fifo_waits_for_xclipse_output_copy_retirement_without_host_stalling(self) -> None:
        """FIFO must not let framegen overwrite an output AHB still being copied."""
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("xclipseOutputCompletionFences_", header)
        self.assertIn("xclipseOutputCompletionFenceSubmitted_", header)
        self.assertIn("retireXclipseOutputCopies", source)

        generic_start = source.index("const bool xclipseFifoPresentation")
        generic_end = source.index("// 4. Generated presentation is opportunistic.", generic_start)
        admission = source[generic_start:generic_end]
        self.assertIn("retireXclipseOutputCopies(generatedFrameCount)", admission)
        self.assertIn("generatedFrameCount = 0", admission)

        generated_start = source.index("// 4. Generated presentation is opportunistic.")
        generated_end = source.index("// 5. Present the real game frame", generated_start)
        generated = source[generated_start:generated_end]
        self.assertIn("xclipseOutputCompletionFence", generated)
        self.assertIn("postCopyBuf.submit(", generated)
        self.assertNotIn("waitContext(", generated)
        self.assertNotIn("vkQueueWaitIdle", generated)

        adreno_start = source.index("// BEGIN ADRENO_364178AF_EXECUTION")
        adreno_end = source.index("// END ADRENO_364178AF_EXECUTION", adreno_start)
        self.assertNotIn("xclipseOutputCompletion", source[adreno_start:adreno_end])

    def test_fix_does_not_add_host_waits_to_fifo_delivery(self) -> None:
        """The Xclipse repair must not reintroduce a per-frame host stall."""
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        generated_start = source.index(
            "// 4. Generated presentation is opportunistic."
        )
        generated_end = source.index(
            "// 5. Present the real game frame", generated_start
        )
        generated = source[generated_start:generated_end]

        self.assertNotIn("waitContext(", generated)
        self.assertNotIn("vkQueueWaitIdle", generated)
        self.assertNotIn("waitQueueIdle_", generated)


    def test_xclipse_fifo_prefers_mailbox_backing_without_touching_adreno(self) -> None:
        """Xclipse logical FIFO uses the proven nonblocking WSI backend when available."""
        header = (ROOT / "include/hooks.hpp").read_text(encoding="utf-8")
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")

        self.assertIn("bool xclipseDevice{false}", header)
        self.assertIn("selectFramegenCompatibilityPath", hooks)

        active_start = hooks.index("VkSwapchainCreateInfoKHR createInfo = *pCreateInfo;")
        active_end = hooks.index("const size_t residentMultiplier", active_start)
        active = hooks[active_start:active_end]
        self.assertIn("deviceInfo->xclipseDevice", active)
        self.assertIn("configuredPresentMode == VK_PRESENT_MODE_FIFO_KHR", active)
        self.assertIn("VK_PRESENT_MODE_MAILBOX_KHR", active)
        self.assertIn("xclipseFifoMailboxBacked", active)

        # Off no longer creates a second source-only swapchain. It bypasses
        # framegen on the resident swapchain, so the already-working Xclipse
        # logical-FIFO/mailbox-backed WSI selection remains untouched.
        self.assertNotIn("const auto createSourceOnly", hooks)
        self.assertNotIn('return createSourceOnly("generation-off")', hooks)

        bypass_start = hooks.index("if (conf.targeted && conf.multiplier <= 1)")
        bypass_end = hooks.index("        try {", bypass_start)
        bypass = hooks[bypass_start:bypass_end]
        self.assertIn("Layer::ovkQueuePresentKHR(queue, pPresentInfo)", bypass)
        self.assertNotIn("xclipseFifoMailboxBacked", bypass)
        self.assertNotIn("VK_PRESENT_MODE_MAILBOX_KHR", bypass)

        adreno_start = (ROOT / "src/context.cpp").read_text(
            encoding="utf-8"
        ).index("// BEGIN ADRENO_364178AF_EXECUTION")
        context = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        adreno_end = context.index("// END ADRENO_364178AF_EXECUTION", adreno_start)
        self.assertNotIn("xclipseFifoMailboxBacked", context[adreno_start:adreno_end])


if __name__ == "__main__":
    unittest.main()
