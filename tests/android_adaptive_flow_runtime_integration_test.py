#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidAdaptiveFlowRuntimeIntegrationTest(unittest.TestCase):
    def test_outer_runtime_creates_adaptive_context_only_in_adaptive_mode(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn('#include "adaptive_flow_controller.hpp"', header)
        self.assertIn("AdaptiveFlowController adaptiveFlowController_", header)
        self.assertIn("AdaptiveFlowPreset adaptiveFlowPreset_", header)

        self.assertIn("AdaptiveFlowController::statesForPreset", source)
        self.assertIn("createAdaptiveContextFromAHB", source)
        self.assertIn("createContextFromAHB", source)
        self.assertIn("1.0F / initialFlowScale", source)
        self.assertIn("float initialFlowScale = conf.flowScale", source)

    def test_completed_gpu_timing_drives_controller_without_new_wait(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("getContextGpuTiming", source)
        self.assertIn("AdaptiveFlowObservation observation", source)
        self.assertIn("timing.valid && !timing.transitionActive", source)
        self.assertIn("requestContextFlowScale", source)
        self.assertIn("getContextFlowScaleState", source)
        self.assertIn("updateAdaptiveFlowGovernor();", source)
        self.assertNotIn("vkDeviceWaitIdle", source)

        # Adaptive LSFG owns cadence first; its transition events suppress Flow
        # evidence instead of making both governors react to the same transient.
        for field in (
            "sourceRateSnapped",
            "costRaised",
            "costBackedOff",
            "costProbe",
            "discontinuityReset",
            "configWarmStart",
        ):
            self.assertIn(field, source)
        self.assertIn(".schedulerTransition = schedulerTransition", source)
        self.assertIn("kAdaptiveFlowCadenceDiscontinuityMs = 250.0", source)
        self.assertIn("!cadenceDiscontinuity", source)

    def test_budget_tracks_adaptive_target_or_fixed_output_period(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn("1000.0 / static_cast<double>(conf.fpsLimit)", source)
        self.assertIn(
            "sourceIntervalMs / static_cast<double>(generatedFrameCount + 1)",
            source,
        )

    def test_fixed_value_is_dormant_while_adaptive_flow_is_active(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")

        self.assertIn(
            "previous.adaptiveFlowScale != next.adaptiveFlowScale",
            hooks,
        )
        self.assertIn(
            "previous.adaptiveFlowPreset != next.adaptiveFlowPreset",
            hooks,
        )
        self.assertIn(
            "!previous.adaptiveFlowScale && !next.adaptiveFlowScale",
            hooks,
        )
        self.assertIn("previous.flowScale != next.flowScale", hooks)

    def test_runtime_telemetry_reports_requested_applied_and_reason(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        fields = (
            "adaptive_flow_preset=",
            "adaptive_flow_target=",
            "adaptive_flow_minimum=",
            "adaptive_flow_requested=",
            "adaptive_flow_active=",
            "adaptive_flow_transition=",
            "adaptive_flow_warmup_remaining=",
            "adaptive_flow_timing_valid=",
            "adaptive_flow_mipmaps_ms=",
            "adaptive_flow_work_ms=",
            "adaptive_flow_lsfg_ms=",
            "adaptive_flow_budget_ms=",
            "adaptive_flow_generation_count=",
            "adaptive_flow_reason=",
        )
        for field in fields:
            self.assertIn(field, source)
            self.assertIn(field, hooks)

        self.assertIn("adaptiveFlowRuntimeSnapshot()", hooks)


if __name__ == "__main__":
    unittest.main()
