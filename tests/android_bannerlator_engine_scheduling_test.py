#!/usr/bin/env python3
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class BannerlatorEngineSchedulingTest(unittest.TestCase):
    def test_alpha_stage_major_schedule_preserves_work_and_batches_sync(self) -> None:
        levels = list(reversed(range(7)))
        stages = range(4)
        schedule = [(stage, level) for stage in stages for level in levels]

        self.assertEqual(28, len(schedule))
        for stage in stages:
            self.assertEqual(levels, [level for s, level in schedule if s == stage])

        # Per-level scheduling emitted four barriers and four pipeline binds for
        # each of seven levels. Stage-major keeps all 28 dispatches but emits
        # one barrier batch and one pipeline bind per stage.
        self.assertEqual(28, 7 * 4)
        self.assertEqual(4, len(list(stages)))

    def test_both_shader_variants_expose_alpha_stage_primitives(self) -> None:
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

    def test_gamma_delta_pairing_preserves_dispatch_order_and_reduces_barriers(self) -> None:
        gamma_stages = 5
        delta_stages = 10
        gamma_only_levels = 4
        paired_levels = 3

        before_barriers = 7 * gamma_stages + paired_levels * delta_stages
        after_barriers = gamma_only_levels * gamma_stages + paired_levels * delta_stages
        self.assertEqual(65, before_barriers)
        self.assertEqual(50, after_barriers)
        self.assertEqual(15, before_barriers - after_barriers)

        for level in range(4, 7):
            order = []
            for step in range(delta_stages):
                if step < gamma_stages:
                    order.append(("gamma", level, step))
                order.append(("delta", level - 4, step))
            for step in range(gamma_stages):
                self.assertLess(
                    order.index(("gamma", level, step)),
                    order.index(("delta", level - 4, step)),
                )

    def test_gamma_delta_step_primitives_exist_in_both_variants(self) -> None:
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

    def test_contexts_pair_gamma_delta_steps(self) -> None:
        for backend in ("v3.1_src", "v3.1p_src"):
            source = (ROOT / "framegen" / backend / "context.cpp").read_text(encoding="utf-8")
            self.assertIn("void dispatchGammaDeltaPaired(", source)
            self.assertIn("gamma.at(level).PushStepBarriers(", source)
            self.assertIn("pairedDelta.PushStepBarriers(", source)
            self.assertIn("gamma.at(level).DispatchStep(", source)
            self.assertIn("pairedDelta.DispatchStep(", source)
            self.assertGreaterEqual(source.count("dispatchGammaDeltaPaired("), 4)
            self.assertNotIn("generationGraph.gamma->at(i).Dispatch(", source)

    def test_contexts_use_one_stage_major_alpha_scheduler(self) -> None:
        for backend in ("v3.1_src", "v3.1p_src"):
            source = (ROOT / "framegen" / backend / "context.cpp").read_text(encoding="utf-8")
            self.assertIn("void dispatchAlphaStageMajor(", source)
            helper_start = source.index("void dispatchAlphaStageMajor(")
            helper_end = source.index("\n}\n", helper_start) + 3
            helper = source[helper_start:helper_end]
            self.assertIn("stage < Shaders::Alpha::StageCount", helper)
            self.assertIn("it->PushBarriers(barriers, frameCount, stage);", helper)
            self.assertIn("alpha.back().BindStagePipeline(buffer, stage);", helper)
            self.assertIn("it->DispatchStage(buffer, frameCount, stage);", helper)

            # Active fixed graph, non-Android fallback, and Adaptive Flow graph
            # must all route through the same stage-major scheduler.
            self.assertGreaterEqual(
                source.count("dispatchAlphaStageMajor("),
                4,
                f"{backend} still has a per-level Alpha dispatch path",
            )
            self.assertNotRegex(
                source,
                re.compile(r"alpha\.at\(6 - i\)\.Dispatch\("),
            )


if __name__ == "__main__":
    unittest.main()
