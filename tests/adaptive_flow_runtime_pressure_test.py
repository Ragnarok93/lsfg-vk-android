#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AdaptiveFlowRuntimePressureContractTest(unittest.TestCase):
    def test_output_fps_cannot_overwrite_source_fps(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn('} else if (key == "source_fps") {', source)
        self.assertIn('} else if (key == "output_fps") {', source)
        self.assertNotIn(
            'key == "source_fps" || key == "output_fps"',
            source,
        )

        source_branch = source.index('} else if (key == "source_fps") {')
        output_branch = source.index('} else if (key == "output_fps") {', source_branch)
        source_block = source[source_branch:output_branch]
        next_branch = source.index('} else if (key == "frame_time_p95_ms") {', output_branch)
        output_block = source[output_branch:next_branch]
        self.assertIn("sample.sourceFps = parsed", source_block)
        self.assertNotIn("sample.sourceFps = parsed", output_block)

    def test_adaptive_fg_pacing_state_reaches_flow_governor_and_logs(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn(".adaptiveFramegenMode = conf.adaptiveFramegen", source)
        self.assertIn(
            ".scheduledGenerationDensity =\n"
            "                adaptiveTelemetry.scheduledGenerationDensity",
            source,
        )
        self.assertIn('" adaptive_density="', source)

    def test_effective_flow_scale_reports_fixed_scale_when_adaptive_flow_is_off(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn("effectiveFlowScale", source)
        self.assertIn("conf.adaptiveFlowScale", source)
        self.assertIn("conf.flowScale", source)
        self.assertIn('" flow_mode="', source)
        self.assertIn('" flow_effective="', source)

    def test_adaptive_fg_has_no_source_health_generation_cap(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        scheduler_header = (ROOT / "include/adaptive_scheduler.hpp").read_text(encoding="utf-8")
        scheduler_source = (ROOT / "src/adaptive_scheduler.cpp").read_text(encoding="utf-8")

        self.assertNotIn("AdaptiveSourceHealthGuard", scheduler_header)
        self.assertNotIn("AdaptiveSourceHealthTelemetry", scheduler_header)
        self.assertNotIn("AdaptiveSourceHealthGuard::", scheduler_source)
        self.assertNotIn("adaptiveSourceHealthGuard_", header)
        self.assertNotIn("adaptiveSourceHealthGuard_", source)
        self.assertNotIn("adaptive_source_health_", source)
        self.assertNotIn("generatedFrameCount = this->adaptiveSourceHealthGuard_.limit(", source)
        self.assertIn(
            "size_t generatedFrameCount = conf.adaptiveFramegen\n"
            "        ? plannedGeneratedFrameCount\n"
            "        : requestedFixedGeneratedFrameCount;",
            source,
        )


if __name__ == "__main__":
    unittest.main()
