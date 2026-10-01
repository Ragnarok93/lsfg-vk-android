#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidMetricsFormatTest(unittest.TestCase):
    def test_adaptive_flow_target_valid_argument_is_not_omitted(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        fmt = '"flow_requested=%.3f flow_transition=%d flow_target_valid=%d "'
        self.assertIn(fmt, source)
        args = (
            "static_cast<double>(this->adaptiveFlowRequestedScale_),\n"
            "                this->adaptiveFlowTransitionPending_ ? 1 : 0,\n"
            "                this->adaptiveFlowOutputTargetValid_ ? 1 : 0,\n"
            "                this->adaptiveFlowWarmupRemaining_,"
        )
        self.assertIn(args, source)

    def test_device_review_logs_expose_context_flow_and_thermal_evidence(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        for marker in (
            '"LSFG_CONTEXT"',
            "context_epoch=%llu build_signature_hash=%llu",
            "flow_mipmaps_ms=%.3f",
            "flow_optical_ms=%.3f",
            "flow_lsfg_ms=%.3f",
            "flow_generation_count=%zu",
            "flow_output_target_fps=%.3f",
            "flow_output_targeted=%d",
            "thermal_status=%d",
            "thermal_valid=%d",
            "thermal_pressure=%d",
        ):
            self.assertIn(marker, source)


if __name__ == "__main__":
    unittest.main()
