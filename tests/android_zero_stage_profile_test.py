#!/usr/bin/env python3
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidZeroStageProfileContractTest(unittest.TestCase):
    def test_timestamp_query_pool_is_optional_and_non_disruptive(self) -> None:
        header_path = ROOT / "framegen/include/core/timestampquerypool.hpp"
        source_path = ROOT / "framegen/src/core/timestampquerypool.cpp"

        self.assertTrue(header_path.exists(), header_path.as_posix())
        self.assertTrue(source_path.exists(), source_path.as_posix())

        header = header_path.read_text(encoding="utf-8")
        source = source_path.read_text(encoding="utf-8")

        self.assertIn("class TimestampQueryPool", header)
        self.assertIn("bool supported() const", header)
        self.assertIn("VK_QUERY_TYPE_TIMESTAMP", source)
        self.assertIn("timestampValidBits", source)
        self.assertIn("timestampPeriod", source)
        self.assertIn("vkCmdResetQueryPool", source)
        self.assertIn("vkCmdWriteTimestamp", source)
        self.assertIn("vkGetQueryPoolResults", source)
        self.assertIn("VK_QUERY_RESULT_64_BIT", source)

    def test_android_build_transform_profiles_zero_stage_without_extra_submit(self) -> None:
        patcher = ROOT / "scripts/apply-zero-stage-profile.py"
        self.assertTrue(patcher.exists(), patcher.as_posix())

        backend_pairs = (
            (
                Path("framegen/v3.1_include/v3_1/context.hpp"),
                Path("framegen/v3.1_src/context.cpp"),
                "quality",
            ),
            (
                Path("framegen/v3.1p_include/v3_1p/context.hpp"),
                Path("framegen/v3.1p_src/context.cpp"),
                "performance",
            ),
        )

        # The checked-in algorithm stays clean; profiling is injected only for
        # the temporary Android profiling build.
        for _, source_rel, backend_name in backend_pairs:
            source = (ROOT / source_rel).read_text(encoding="utf-8")
            self.assertNotIn(
                f"zero-stage-profile backend={backend_name}",
                source,
                source_rel.as_posix(),
            )

        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            for header_rel, source_rel, _ in backend_pairs:
                for rel in (header_rel, source_rel):
                    target = temp_root / rel
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(ROOT / rel, target)

            subprocess.run(
                [sys.executable, str(patcher), "--root", str(temp_root)],
                check=True,
            )
            # Prove the transform is idempotent as local/CI build scripts can
            # be invoked more than once in the same checkout.
            subprocess.run(
                [sys.executable, str(patcher), "--root", str(temp_root)],
                check=True,
            )

            for header_rel, source_rel, backend_name in backend_pairs:
                header = (temp_root / header_rel).read_text(encoding="utf-8")
                source = (temp_root / source_rel).read_text(encoding="utf-8")
                present_start = source.index("void Context::present")
                wait_start = source.index("bool Context::waitForLastPresent", present_start)
                present = source[present_start:wait_start]

                self.assertIn('#include "core/timestampquerypool.hpp"', header)
                self.assertIn("Core::TimestampQueryPool zeroStageQueryPool", header)
                self.assertIn("std::array<double, 8> zeroStageProfileTotalsMs", header)
                self.assertIn("uint32_t zeroStageProfileSamples", header)
                self.assertEqual(
                    source.count("Core::TimestampQueryPool(vk.device, 9)"),
                    2,
                    source_rel.as_posix(),
                )

                self.assertIn(
                    "const bool profileZeroStage = generationCount == 0",
                    present,
                    source_rel.as_posix(),
                )
                self.assertIn("zeroStageQueryPool.reset", present, source_rel.as_posix())
                self.assertGreaterEqual(
                    present.count("zeroStageQueryPool.write"),
                    3,
                    source_rel.as_posix(),
                )
                self.assertIn("this->mipmaps.Dispatch", present, source_rel.as_posix())
                self.assertIn("this->alpha.at(6 - i).Dispatch", present, source_rel.as_posix())
                self.assertIn("zeroStageQueryPool.durationsMs", present, source_rel.as_posix())
                self.assertIn(
                    f"zero-stage-profile backend={backend_name}",
                    present,
                    source_rel.as_posix(),
                )
                self.assertIn("samples=", present, source_rel.as_posix())
                self.assertIn("mipmaps_avg_ms=", present, source_rel.as_posix())
                for alpha in range(6, -1, -1):
                    self.assertIn(f"alpha{alpha}_avg_ms=", present, source_rel.as_posix())
                self.assertIn("gpu_total_avg_ms=", present, source_rel.as_posix())

                zero_start = present.index("if (generationCount == 0)")
                second_stage = present.index(
                    "for (size_t pass = 0; pass < generationCount; pass++)", zero_start
                )
                zero_block = present[zero_start:second_stage]
                self.assertEqual(
                    zero_block.count("data.cmdBuffer1.submit"),
                    1,
                    source_rel.as_posix(),
                )


if __name__ == "__main__":
    unittest.main()
