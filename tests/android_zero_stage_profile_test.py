#!/usr/bin/env python3
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

    def test_zero_cycle_profiles_mipmaps_and_each_alpha_without_extra_submit(self) -> None:
        backend_pairs = (
            (
                ROOT / "framegen/v3.1_include/v3_1/context.hpp",
                ROOT / "framegen/v3.1_src/context.cpp",
                "quality",
            ),
            (
                ROOT / "framegen/v3.1p_include/v3_1p/context.hpp",
                ROOT / "framegen/v3.1p_src/context.cpp",
                "performance",
            ),
        )

        for header_path, source_path, backend_name in backend_pairs:
            header = header_path.read_text(encoding="utf-8")
            source = source_path.read_text(encoding="utf-8")
            present_start = source.index("void Context::present")
            wait_start = source.index("bool Context::waitForLastPresent", present_start)
            present = source[present_start:wait_start]

            self.assertIn('#include "core/timestampquerypool.hpp"', header)
            self.assertIn("Core::TimestampQueryPool zeroStageQueryPool", header)
            self.assertIn("std::array<double, 8> zeroStageProfileTotalsMs", header)
            self.assertIn("uint32_t zeroStageProfileSamples", header)

            self.assertIn(
                "const bool profileZeroStage = generationCount == 0",
                present,
                source_path.as_posix(),
            )
            self.assertIn("zeroStageQueryPool.reset", present, source_path.as_posix())
            self.assertGreaterEqual(
                present.count("zeroStageQueryPool.write"),
                3,
                source_path.as_posix(),
            )
            self.assertIn("this->mipmaps.Dispatch", present, source_path.as_posix())
            self.assertIn("this->alpha.at(6 - i).Dispatch", present, source_path.as_posix())
            self.assertIn("zeroStageQueryPool.durationsMs", present, source_path.as_posix())
            self.assertIn(
                f'zero-stage-profile backend={backend_name}',
                present,
                source_path.as_posix(),
            )
            self.assertIn("samples=", present, source_path.as_posix())
            self.assertIn("mipmaps_avg_ms=", present, source_path.as_posix())
            for alpha in range(6, -1, -1):
                self.assertIn(f"alpha{alpha}_avg_ms=", present, source_path.as_posix())
            self.assertIn("gpu_total_avg_ms=", present, source_path.as_posix())

            zero_start = present.index("if (generationCount == 0)")
            second_stage = present.index(
                "for (size_t pass = 0; pass < generationCount; pass++)", zero_start
            )
            zero_block = present[zero_start:second_stage]
            self.assertEqual(
                zero_block.count("data.cmdBuffer1.submit"),
                1,
                source_path.as_posix(),
            )


if __name__ == "__main__":
    unittest.main()
