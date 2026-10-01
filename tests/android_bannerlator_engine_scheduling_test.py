#!/usr/bin/env python3
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class BannerlatorEngineSchedulingTest(unittest.TestCase):
    def test_barrier_batch_contract_matches_inline_builder_capacity(self) -> None:
        header = (ROOT / "framegen/include/common/utils.hpp").read_text(encoding="utf-8")
        match = re.search(r"kInlineBarrierCapacity\s*=\s*(\d+)", header)
        self.assertIsNotNone(match)
        capacity = int(match.group(1))

        per_chain_maxima = {
            "quality-alpha": 8,
            "performance-alpha": 4,
            "quality-gamma": 12,
            "quality-delta": 12,
            "performance-gamma": 8,
            "performance-delta": 8,
        }
        for name, barrier_count in per_chain_maxima.items():
            self.assertLessEqual(
                barrier_count, capacity,
                f"{name} exceeds BarrierBuilder capacity",
            )

        # These were the unsafe merged batches introduced by the optimization.
        # They must not be used while BarrierBuilder remains a 16-entry inline builder.
        self.assertGreater(56, capacity)
        self.assertGreater(24, capacity)

    def test_both_shader_variants_keep_stage_primitives_available(self) -> None:
        variants = (
            ("v3.1_include/v3_1/shaders/alpha.hpp", "v3.1_src/shaders/alpha.cpp"),
            ("v3.1p_include/v3_1p/shaders/alpha.hpp", "v3.1p_src/shaders/alpha.cpp"),
        )
        for header_rel, source_rel in variants:
            header = (ROOT / "framegen" / header_rel).read_text(encoding="utf-8")
            source = (ROOT / "framegen" / source_rel).read_text(encoding="utf-8")
            self.assertIn("static constexpr size_t StageCount = 4;", header)
            self.assertIn("void PushBarriers(Utils::BarrierBuilder& barriers", header)
            self.assertIn("void BindStagePipeline(", header)
            self.assertIn("void DispatchStage(", header)
            self.assertIn("void Alpha::PushBarriers(", source)
            self.assertIn("void Alpha::BindStagePipeline(", source)
            self.assertIn("void Alpha::DispatchStage(", source)

    def test_gamma_delta_step_primitives_remain_available(self) -> None:
        for variant in ("v3.1", "v3.1p"):
            namespace = "v3_1" if variant == "v3.1" else "v3_1p"
            for shader, stages in (("gamma", 5), ("delta", 10)):
                header = (
                    ROOT / "framegen" / f"{variant}_include" / namespace / "shaders" / f"{shader}.hpp"
                ).read_text(encoding="utf-8")
                source = (
                    ROOT / "framegen" / f"{variant}_src" / "shaders" / f"{shader}.cpp"
                ).read_text(encoding="utf-8")
                class_name = shader.capitalize()
                self.assertIn(f"static constexpr size_t StageCount = {stages};", header)
                self.assertIn("void PushStepBarriers(", header)
                self.assertIn("void DispatchStep(", header)
                self.assertIn(f"void {class_name}::PushStepBarriers(", source)
                self.assertIn(f"void {class_name}::DispatchStep(", source)

    def test_contexts_keep_alpha_barriers_per_mip_level(self) -> None:
        for backend in ("v3.1_src", "v3.1p_src"):
            source = (ROOT / "framegen" / backend / "context.cpp").read_text(encoding="utf-8")
            self.assertNotIn("dispatchAlphaStageMajor(", source)
            self.assertRegex(source, re.compile(r"alpha\.at\(6 - i\)\.Dispatch\("))
            self.assertRegex(source, re.compile(r"graph\.alpha->at\(6 - i\)\.Dispatch\("))

    def test_contexts_keep_gamma_delta_barriers_independent(self) -> None:
        for backend in ("v3.1_src", "v3.1p_src"):
            source = (ROOT / "framegen" / backend / "context.cpp").read_text(encoding="utf-8")
            self.assertNotIn("dispatchGammaDeltaPaired(", source)
            self.assertIn("generationGraph.gamma->at(i).Dispatch(", source)
            self.assertIn("generationGraph.delta->at(i - 4).Dispatch(", source)
            self.assertIn("this->gamma.at(i).Dispatch(", source)
            self.assertIn("this->delta.at(i - 4).Dispatch(", source)


if __name__ == "__main__":
    unittest.main()
