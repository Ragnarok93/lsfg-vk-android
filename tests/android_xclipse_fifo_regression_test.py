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
            generated_end,
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


if __name__ == "__main__":
    unittest.main()
