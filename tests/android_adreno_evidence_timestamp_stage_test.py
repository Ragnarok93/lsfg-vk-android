#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidAdrenoEvidenceTimestampStageTest(unittest.TestCase):
    def test_transport_timestamps_use_stage_aware_queries(self) -> None:
        main = (ROOT / "scripts/apply-adreno-evidence-profile.py").read_text(encoding="utf-8")
        helper = (ROOT / "scripts/adreno_evidence_framegen.py").read_text(encoding="utf-8")

        self.assertIn("patch_timestamp_query_pool", main)
        self.assertIn("framegen/include/core/timestampquerypool.hpp", main)
        self.assertIn("framegen/src/core/timestampquerypool.cpp", main)
        self.assertIn("writeAtStage", helper)
        self.assertIn("VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT", helper)
        self.assertIn("VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT", helper)

        # Compute-stage timestamps remain the default for shader boundaries;
        # only transport boundaries need top/bottom-of-pipe placement.
        self.assertIn("generatedPreQueryPool.write(data.cmdBuffer1.handle(), 2)", helper)
        self.assertIn("generatedPassProfile->write(buf2.handle(), generatedPassQueryIndex++)", helper)

    def test_b12_dual_stage_profile_is_low_overhead_and_adaptive_compatible(self) -> None:
        transform = (ROOT / "scripts/apply-b12-dual-stage-profile.py").read_text(encoding="utf-8")
        build = (ROOT / "scripts/build/android.sh").read_text(encoding="utf-8")

        self.assertIn("LSFGVK_B12_DUAL_STAGE_PROFILE", build)
        self.assertIn("apply-b12-dual-stage-profile.py", build)
        self.assertIn("b12-stage-profile", transform)
        self.assertIn("b12MipmapsQueryPool", transform)
        self.assertIn("b12Beta4QueryPool", transform)
        self.assertIn("mipmaps_avg_ms=", transform)
        self.assertIn("beta4_avg_ms=", transform)
        self.assertIn("generationCount > 0", transform)

        # B12 is deliberately a two-stage timer that can ride along with the
        # normal adaptive runtime.  It must not turn on the much larger full
        # generated-stage profiler or alter shader bytecode/dispatch geometry.
        self.assertNotIn("generated-stage-profile backend=", transform)
        self.assertNotIn("OpControlBarrier", transform)
        self.assertNotIn("replace_shader", transform)
        self.assertNotIn("local_size", transform.lower())


if __name__ == "__main__":
    unittest.main()
