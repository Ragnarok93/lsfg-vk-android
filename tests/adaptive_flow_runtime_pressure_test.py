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


if __name__ == "__main__":
    unittest.main()
