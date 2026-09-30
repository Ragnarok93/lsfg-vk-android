#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AdaptiveFlowShadowTimingTest(unittest.TestCase):
    def test_shadow_preprocess_has_independent_gpu_timing(self) -> None:
        public = (ROOT / "framegen/public/lsfg_backend.hpp").read_text(encoding="utf-8")
        for marker in (
            "shadowMipmapsMs",
            "shadowAlphaMs",
            "shadowPreprocessMs",
            "shadowPreprocessSubmitted",
        ):
            self.assertIn(marker, public)

        variants = (
            ("v3.1_src", "v3.1_include", "v3_1"),
            ("v3.1p_src", "v3.1p_include", "v3_1p"),
        )
        for backend, include_dir, namespace_dir in variants:
            source = (ROOT / "framegen" / backend / "context.cpp").read_text(encoding="utf-8")
            header = (
                ROOT / "framegen" / include_dir / namespace_dir / "context.hpp"
            ).read_text(encoding="utf-8")
            self.assertIn("adaptiveFlowShadowTimingQueryPool", header)
            self.assertIn("Core::TimestampQueryPool(vk.device, 3)", source)
            self.assertIn("shadowTimingPool->write(data.cmdBuffer1.handle(), 0)", source)
            self.assertIn("shadowTimingPool->write(data.cmdBuffer1.handle(), 2)", source)
            self.assertIn("shadowDurations.size() == 2", source)
            self.assertIn("shadowMipmapsMs = shadowDurations.at(0)", source)
            self.assertIn("shadowAlphaMs = shadowDurations.at(1)", source)

            warmup = source.index("if (this->pendingFlowWarmupFrames_ + 1")
            final_handoff = source.index("} else {", warmup)
            overlap = source[warmup:final_handoff]
            self.assertIn("activeGraph.beta->Dispatch", overlap)
            self.assertNotIn("pendingGraph.beta->Dispatch", overlap)

    def test_outer_runtime_reports_shadow_and_context_build_cost(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        for marker in (
            "context_build_ms=",
            "shadow_preprocess_submitted=",
            "shadow_mipmaps_ms=",
            "shadow_alpha_ms=",
            "shadow_preprocess_ms=",
        ):
            self.assertIn(marker, source)


if __name__ == "__main__":
    unittest.main()
