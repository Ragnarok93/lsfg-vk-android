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


if __name__ == "__main__":
    unittest.main()
