#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidBarrierScopeRegressionTest(unittest.TestCase):
    def test_read_to_write_uses_execution_dependency_only(self) -> None:
        source = (ROOT / "framegen/src/common/utils.cpp").read_text(encoding="utf-8")
        r2w = source.substring(
            source.index("BarrierBuilder& BarrierBuilder::addR2W"),
            source.index("BarrierBuilder& BarrierBuilder::addW2R"),
        )
        self.assertIn("VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT", r2w)
        self.assertIn("VK_ACCESS_2_NONE", r2w)
        self.assertIn("VK_ACCESS_2_SHADER_WRITE_BIT", r2w)
        self.assertNotIn("VK_ACCESS_2_SHADER_READ_BIT", r2w)

    def test_write_to_read_visibility_remains_intact(self) -> None:
        source = (ROOT / "framegen/src/common/utils.cpp").read_text(encoding="utf-8")
        w2r = source.substring(
            source.index("BarrierBuilder& BarrierBuilder::addW2R"),
            source.index("void BarrierBuilder::build"),
        )
        self.assertIn("VK_ACCESS_2_SHADER_WRITE_BIT", w2r)
        self.assertIn("VK_ACCESS_2_SHADER_READ_BIT", w2r)
        self.assertIn("VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT", w2r)

    def test_barriers_remain_image_and_subresource_local(self) -> None:
        source = (ROOT / "framegen/src/common/utils.cpp").read_text(encoding="utf-8")
        for marker in ("BarrierBuilder& BarrierBuilder::addR2W", "BarrierBuilder& BarrierBuilder::addW2R"):
            start = source.index(marker)
            end = source.index("return *this;", start)
            body = source[start:end]
            self.assertIn(".image = image.handle()", body)
            self.assertIn(".levelCount = 1", body)
            self.assertIn(".layerCount = 1", body)
            self.assertNotIn("VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT", body)


if __name__ == "__main__":
    unittest.main()
