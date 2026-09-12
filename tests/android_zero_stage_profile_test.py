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

    def test_android_build_transform_splits_mipmaps_barrier_and_compute_without_extra_submit(self) -> None:
        patcher = ROOT / "scripts/apply-zero-stage-profile.py"
        self.assertTrue(patcher.exists(), patcher.as_posix())

        backend_sets = (
            (
                Path("framegen/v3.1_include/v3_1/context.hpp"),
                Path("framegen/v3.1_src/context.cpp"),
                Path("framegen/v3.1_include/v3_1/shaders/mipmaps.hpp"),
                Path("framegen/v3.1_src/shaders/mipmaps.cpp"),
                "quality",
            ),
            (
                Path("framegen/v3.1p_include/v3_1p/context.hpp"),
                Path("framegen/v3.1p_src/context.cpp"),
                Path("framegen/v3.1p_include/v3_1p/shaders/mipmaps.hpp"),
                Path("framegen/v3.1p_src/shaders/mipmaps.cpp"),
                "performance",
            ),
        )

        # The checked-in algorithm stays clean; profiling is injected only for
        # the temporary Android profiling build.
        for _, context_rel, _, mipmaps_source_rel, backend_name in backend_sets:
            context_source = (ROOT / context_rel).read_text(encoding="utf-8")
            mipmaps_source = (ROOT / mipmaps_source_rel).read_text(encoding="utf-8")
            self.assertNotIn(
                f"zero-stage-profile backend={backend_name}",
                context_source,
                context_rel.as_posix(),
            )
            self.assertNotIn(
                f"zero-stage-profile-config backend={backend_name}",
                mipmaps_source,
                mipmaps_source_rel.as_posix(),
            )

        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            for context_header_rel, context_rel, mipmaps_header_rel, mipmaps_source_rel, _ in backend_sets:
                for rel in (
                    context_header_rel,
                    context_rel,
                    mipmaps_header_rel,
                    mipmaps_source_rel,
                ):
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

            for context_header_rel, context_rel, mipmaps_header_rel, mipmaps_source_rel, backend_name in backend_sets:
                context_header = (temp_root / context_header_rel).read_text(encoding="utf-8")
                context_source = (temp_root / context_rel).read_text(encoding="utf-8")
                mipmaps_header = (temp_root / mipmaps_header_rel).read_text(encoding="utf-8")
                mipmaps_source = (temp_root / mipmaps_source_rel).read_text(encoding="utf-8")
                present_start = context_source.index("void Context::present")
                wait_start = context_source.index("bool Context::waitForLastPresent", present_start)
                present = context_source[present_start:wait_start]

                self.assertIn('#include "core/timestampquerypool.hpp"', context_header)
                self.assertIn("Core::TimestampQueryPool zeroStageQueryPool", context_header)
                self.assertIn("std::array<double, 9> zeroStageProfileTotalsMs", context_header)
                self.assertIn("uint32_t zeroStageProfileSamples", context_header)
                self.assertEqual(
                    context_source.count("Core::TimestampQueryPool(vk.device, 10)"),
                    2,
                    context_rel.as_posix(),
                )

                self.assertIn('#include "core/timestampquerypool.hpp"', mipmaps_header)
                self.assertIn("Core::TimestampQueryPool* profilePool", mipmaps_header)
                self.assertIn("uint32_t postBarrierQueryIndex", mipmaps_header)
                self.assertIn("profilePool->write(buf.handle(), postBarrierQueryIndex)", mipmaps_source)
                self.assertIn(
                    f"zero-stage-profile-config backend={backend_name}",
                    mipmaps_source,
                    mipmaps_source_rel.as_posix(),
                )
                for field in (
                    "source_extent=",
                    "flow_scale=",
                    "flow_extent=",
                    "dispatch_grid=",
                ):
                    self.assertIn(field, mipmaps_source, mipmaps_source_rel.as_posix())

                barrier_end = mipmaps_source.index(".build();")
                timestamp_after_barrier = mipmaps_source.index(
                    "profilePool->write(buf.handle(), postBarrierQueryIndex)", barrier_end
                )
                dispatch = mipmaps_source.index("buf.dispatch", timestamp_after_barrier)
                self.assertLess(timestamp_after_barrier, dispatch, mipmaps_source_rel.as_posix())

                self.assertIn(
                    "const bool profileZeroStage = generationCount == 0",
                    present,
                    context_rel.as_posix(),
                )
                self.assertIn("zeroStageQueryPool.reset", present, context_rel.as_posix())
                self.assertIn(
                    "profileZeroStage ? &this->zeroStageQueryPool : nullptr",
                    present,
                    context_rel.as_posix(),
                )
                self.assertIn("this->mipmaps.Dispatch", present, context_rel.as_posix())
                self.assertIn("this->alpha.at(6 - i).Dispatch", present, context_rel.as_posix())
                self.assertIn("zeroStageQueryPool.durationsMs", present, context_rel.as_posix())
                self.assertIn(
                    f"zero-stage-profile backend={backend_name}",
                    present,
                    context_rel.as_posix(),
                )
                self.assertIn("samples=", present, context_rel.as_posix())
                self.assertIn("mipmaps_avg_ms=", present, context_rel.as_posix())
                self.assertIn("mipmaps_barrier_avg_ms=", present, context_rel.as_posix())
                self.assertIn("mipmaps_compute_avg_ms=", present, context_rel.as_posix())
                for alpha in range(6, -1, -1):
                    self.assertIn(f"alpha{alpha}_avg_ms=", present, context_rel.as_posix())
                self.assertIn("gpu_total_avg_ms=", present, context_rel.as_posix())

                command_end = present.index("data.cmdBuffer1.end()")
                zero_start = present.index("if (generationCount == 0) {", command_end)
                zero_end = present.index("        return;", zero_start) + len("        return;")
                zero_block = present[zero_start:zero_end]
                self.assertEqual(
                    zero_block.count("data.cmdBuffer1.submit"),
                    1,
                    context_rel.as_posix(),
                )


if __name__ == "__main__":
    unittest.main()
