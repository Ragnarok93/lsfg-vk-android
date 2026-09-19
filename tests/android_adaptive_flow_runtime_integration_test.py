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
        self.assertIn("adaptive-flow-fallback mode=fixed-target", source)
        self.assertIn("adaptiveFlowRuntimeAvailable_", source)
        self.assertIn("!this->adaptiveFlowRuntimeAvailable_", source)
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

        # Adaptive LSFG still owns cadence. Ordinary scheduler transitions hold
        # local Flow evidence, while sustained whole-device GPU pressure is
        # allowed to survive the hold so the two governors do not become blind.
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

    def test_global_gpu_pressure_is_sampled_out_of_band_without_rebuild(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn('runtime-pressure.txt', source)
        self.assertIn("readRuntimePressure", source)
        self.assertIn("std::chrono::milliseconds(500)", source)
        self.assertIn("std::chrono::seconds(2)", source)
        self.assertIn("adaptiveFlowGlobalPressureValid_", header)
        self.assertIn("adaptiveFlowGlobalGpuUsagePercent_", header)
        self.assertIn("adaptiveFlowGeneratedTimingValid_", header)
        self.assertIn("adaptiveFlowRetainedTotalLsfgMs_", header)
        self.assertIn("adaptiveFlowLastObservedLateDrops_", header)

        # Whole-device pressure is evidence only. It must not touch the source
        # timeline, sleep the present thread, or cause a configuration reload.
        reader_start = source.index("RuntimePressureSample readRuntimePressure")
        reader_end = source.index("VkImageSubresourceRange", reader_start)
        reader = source[reader_start:reader_end]
        self.assertNotIn("updateConfig", reader)
        self.assertNotIn("sleep_for", reader)
        self.assertNotIn("sourceTimeline_", reader)

    def test_history_only_cycles_can_pressure_flow_but_never_prove_headroom(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        controller = (ROOT / "src/adaptive_flow_controller.cpp").read_text(
            encoding="utf-8"
        )

        self.assertIn("generatedWorkSample", source)
        self.assertIn("adaptiveFlowRetainedWorkMs_", source)
        self.assertIn("adaptiveFlowRetainedTotalLsfgMs_", source)
        self.assertIn(".generatedWorkSample = generatedWorkSample", source)

        self.assertIn("const bool localPressure", controller)
        self.assertIn("observation.generatedWorkSample", controller)
        self.assertIn("globalPressure", controller)
        self.assertIn("&& observation.generatedWorkSample", controller)
        self.assertIn("kGlobalGpuPressurePercent = 96.0", controller)
        self.assertIn("kGlobalGpuRecoveryPercent = 88.0", controller)

    def test_global_pressure_uses_output_deficit_and_late_drop_evidence(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("adaptiveOutputDeficit", source)
        self.assertIn("fixedOutputDeficit", source)
        self.assertIn("metrics.totalGeneratedLateDrops", source)
        self.assertIn("syntheticDropPressure", source)
        self.assertIn(".globalGpuUsagePercent =", source)
        self.assertIn(".globalPressureValid =", source)
        self.assertIn(".outputDeficit = outputDeficit", source)
        self.assertIn(".syntheticDropPressure = syntheticDropPressure", source)

    def test_budget_tracks_adaptive_target_or_fixed_output_period(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn("1000.0 / static_cast<double>(conf.fpsLimit)", source)
        self.assertIn(
            "sourceIntervalMs / static_cast<double>(generatedFrameCount + 1)",
            source,
        )

    def test_deadline_admission_protects_source_without_catchup_debt(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("DeadlineAdmissionPredictor deadlineAdmissionPredictor_", header)
        self.assertIn("deadlineBatchDecision_", header)
        self.assertIn("Active deadline admission", source)
        self.assertIn("plannedGeneratedFrameCount", source)
        self.assertIn("generatedFrameCount = 1", source)
        self.assertIn("sourceHistoryWarmupActive", source)
        self.assertIn("admittedGeneratedFrameCount", source)
        self.assertIn(
            "generatedFrameCount = admittedGeneratedFrameCount",
            source,
        )
        self.assertIn(
            "candidate > 0; --candidate",
            source,
        )
        self.assertIn(
            "slot + 1, slotBudgetMs",
            source,
        )
        self.assertIn("plannedGeneratedFrameCount > 0", source)
        self.assertIn("generatedFrameCount == 0", source)
        self.assertIn("AndroidFrameCycleMode::HistoryOnly", source)
        self.assertIn("deadlineAdmissionPredictor_.predict", source)
        self.assertIn("deadlineAdmissionPredictor_.observe", source)
        self.assertIn("deadline_planned_generated=", source)
        self.assertIn("deadline_admitted_generated=", source)
        self.assertIn("interpolation_denominator=", source)
        self.assertIn("interpolationGenerationCount", source)
        self.assertIn(
            "static_cast<double>(interpolationGenerationCount + 1)",
            source,
        )
        self.assertIn("deadline_prediction_error_avg_ms=", source)

        # Admission can lower synthetic density, but it cannot alter the source
        # timeline or create pacing/catch-up work of its own.
        admission_start = source.index("Active deadline admission")
        governor_start = source.index(
            "const auto updateAdaptiveFlowGovernor", admission_start
        )
        admission = source[admission_start:governor_start]
        self.assertNotIn("sleep_for", admission)
        self.assertNotIn("sourceDesiredTimeNs_ =", admission)
        self.assertNotIn("fractionalOpportunityPhase_", admission)


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
            "adaptive_flow_global_pressure_valid=",
            "adaptive_flow_global_gpu_percent=",
            "adaptive_flow_global_output_fps=",
            "adaptive_flow_global_p95_ms=",
            "adaptive_flow_global_slow_ratio=",
            "adaptive_flow_global_pressure=",
            "adaptive_flow_output_deficit=",
            "adaptive_flow_synthetic_drop_pressure=",
            "adaptive_flow_reason=",
        )
        for field in fields:
            self.assertIn(field, source)
            self.assertIn(field, hooks)

        self.assertIn("adaptiveFlowRuntimeSnapshot()", hooks)


    def test_adaptive_framegen_preserves_configured_present_mode(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        context = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        pacing_start = hooks.index("bool adaptivePresentationPacing")
        pacing_end = hooks.index("bool requiresSwapchainRecreation", pacing_start)
        pacing = hooks[pacing_start:pacing_end]
        self.assertIn("return false;", pacing)

        create_start = hooks.index("const auto configuredPresentMode")
        create_end = hooks.index(
            'std::cerr << "lsfg-vk: init stage=swapchain-downstream-create-begin',
            create_start,
        )
        create_block = hooks[create_start:create_end]
        self.assertIn(
            "const auto configuredPresentMode = Config::activeConf.e_present;",
            create_block,
        )
        self.assertNotIn(
            "configuredPresentMode = adaptivePacing",
            create_block,
        )
        self.assertNotIn(
            "VK_PRESENT_MODE_FIFO_KHR);",
            create_block,
        )

        desired_start = hooks.index("const VkPresentModeKHR desiredPresentMode")
        desired_end = hooks.index("if (configuredPresent != desiredPresentMode)", desired_start)
        desired_block = hooks[desired_start:desired_end]
        self.assertIn("desiredPresentMode = conf.e_present", desired_block)
        self.assertNotIn("adaptivePresentationPacing(conf)", desired_block)

        # Keep display-timing capability detection available, but do not let it
        # alter the restored known-good WSI pacing contract.
        self.assertIn("adaptiveDisplayTimingEnabled_ = false", context)
        self.assertIn("fifo_override=0", context)


if __name__ == "__main__":
    unittest.main()
