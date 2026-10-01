#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AdaptiveFlowShadowTimingTest(unittest.TestCase):
    def test_seeded_handoff_uses_normal_gpu_timing_without_shadow_overlap(self) -> None:
        # Keep the public shadow fields for telemetry/backward compatibility,
        # but the release path must no longer submit or time a second shadow
        # preprocess alongside generated work.
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
            source = (ROOT / "framegen" / backend / "context.cpp").read_text(
                encoding="utf-8"
            )
            header = (
                ROOT / "framegen" / include_dir / namespace_dir / "context.hpp"
            ).read_text(encoding="utf-8")

            self.assertIn("dispatchAdaptiveFlowSeedHistory", header)
            self.assertIn("dispatchAdaptiveFlowSeedHistory", source)
            self.assertIn(
                "pendingGraph, adaptiveFlowTimingPool",
                source,
            )
            self.assertIn("pendingGraph.beta->Dispatch", source)

            self.assertNotIn("shadowTimingPool->write", source)
            self.assertNotIn("adaptive-flow-shadow-deferred", source)
            self.assertNotIn("shadowBudgetAvailable", source)

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
