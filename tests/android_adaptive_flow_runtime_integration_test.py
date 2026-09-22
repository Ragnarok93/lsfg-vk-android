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
        self.assertIn("timingSessionMatches", source)
        self.assertIn("timingFresh", source)
        self.assertIn("timing.transitionActive", source)
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
        self.assertIn("adaptiveFlowLastObservedComputeDrops_", header)
        self.assertIn("adaptiveFlowLastObservedWsiDrops_", header)

        # Whole-device pressure is evidence only. It must not touch the source
        # timeline, sleep the present thread, or cause a configuration reload.
        reader_start = source.index("RuntimePressureSample readRuntimePressure")
        reader_end = source.index("VkImageSubresourceRange", reader_start)
        reader = source[reader_start:reader_end]
        self.assertNotIn("updateConfig", reader)
        self.assertNotIn("sleep_for", reader)
        self.assertNotIn("sourceTimeline_", reader)

    def test_history_only_uses_retained_generated_timing_for_guarded_recovery(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        controller = (ROOT / "src/adaptive_flow_controller.cpp").read_text(
            encoding="utf-8"
        )

        self.assertIn("generatedWorkSample", source)
        self.assertIn("adaptiveFlowRetainedWorkMs_", source)
        self.assertIn("adaptiveFlowRetainedTotalLsfgMs_", source)
        self.assertIn(".generatedWorkSample = generatedWorkSample", source)

        # Current zero-generation timing cannot masquerade as cheap generated
        # work. Pressure still requires a real generated sample, while recovery
        # may use the retained real generated timing only with fresh global
        # headroom and a met LSFG output target.
        self.assertIn("const bool computePressure", controller)
        self.assertIn("observation.computeDeadlinePressure", controller)
        self.assertIn("const bool wsiFlowPressure", controller)
        self.assertIn("observation.wsiPresentationPressure", controller)
        self.assertIn("observation.generatedWorkSample", controller)
        self.assertIn("retainedHistoryRecoveryEligible", controller)
        self.assertIn("!observation.generatedWorkSample", controller)
        self.assertIn("observation.globalPressureValid", controller)
        self.assertIn("globalRecoveryHeadroom", controller)
        self.assertIn("recoveryTimingEligible", controller)
        self.assertIn("kGlobalGpuPressurePercent = 96.0", controller)
        self.assertIn("kGlobalGpuRecoveryPercent = 88.0", controller)

    def test_flow_control_uses_rolling_lsfg_output_and_split_pressure_domains(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("LsfgOutputCadenceTracker lsfgOutputCadenceTracker_", header)
        self.assertIn("Previous completed one-second LSFG metrics window", header)
        self.assertIn("const auto& outputCadence", source)
        self.assertIn("outputCadence.deficitConfirmed", source)
        self.assertNotIn("adaptiveOutputSampleMatches", source)

        self.assertIn("adaptiveFlowLastObservedComputeDrops_", header)
        self.assertIn("adaptiveFlowLastObservedWsiDrops_", header)
        self.assertIn("computeDropPressure", source)
        self.assertIn("wsiPresentationPressure", source)
        self.assertIn(".computeDeadlinePressure = computeDropPressure", source)
        self.assertIn(".wsiPresentationPressure = wsiPresentationPressure", source)
        self.assertIn(".syntheticDropPressure = false", source)
        self.assertIn(".wsiLossRate = presentationCapacity.wsiRejectionRatio", source)

        deficit_start = source.index("const auto& outputCadence")
        deficit_end = source.index(
            "const auto updateAdaptiveFlowGovernor", deficit_start
        )
        deficit = source[deficit_start:deficit_end]
        self.assertNotIn("adaptiveFlowGlobalOutputFps_", deficit)
        self.assertNotIn("metrics.lastWindowOutputFps", deficit)

    def test_logcat_diagnostics_include_runtime_session_and_config_epoch(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("runtimeSessionId_", header)
        self.assertIn("runtimeConfigSignature_", header)
        self.assertIn("configRevision_", header)
        self.assertIn("processRuntimeSessionId", source)
        self.assertIn("runtimeDiagnosticConfigSignature", source)
        self.assertEqual(
            source.count('"runtime_session_id=%llu config_revision=%llu "'),
            3,
        )
        for tag in ('"LSFG_METRICS"', '"LSFG_EVENT"', '"LSFG_FLOW"'):
            self.assertIn(tag, source)

        # Config epochs must be process-monotonic rather than restarting at
        # revision 1 whenever a swapchain/context is recreated.
        self.assertIn("nextRuntimeConfigRevision", source)
        self.assertNotIn("this->configRevision_ = 1;", source)

    def test_budget_tracks_adaptive_target_or_fixed_output_period(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn("1000.0 / static_cast<double>(conf.fpsLimit)", source)
        self.assertIn("if (conf.adaptiveFramegen)\n        return sourceIntervalMs;", source)
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
            "interpolationGenerationCount = generatedFrameCount",
            source,
        )
        self.assertIn(
            "static_cast<double>(candidate + 1)",
            source,
        )
        self.assertIn(
            "static_cast<double>(interpolationGenerationCount + 1)",
            source,
        )
        self.assertIn("deadline_prediction_error_avg_ms=", source)
        self.assertIn("deadline_delivery_reserve_ms=", source)
        self.assertIn("deadline_effective_budget_ms=", source)
        self.assertIn("admission_rejects=", source)
        self.assertIn("generated_deadline_drops=", source)
        self.assertIn("generated_wsi_drops=", source)
        self.assertIn("observeDeliveryMiss", source)
        self.assertIn("observeDeliverySuccess", source)

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


    def test_capacity_hint_and_presentation_cap_are_pre_dispatch(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("GeneratedPresentationCapacityTracker", header)
        self.assertIn("setSafeGenerationHint", source)
        self.assertIn("safeGenerationHint(", source)
        self.assertIn("generatedPresentationCapacityTracker_.limit", source)
        cap_start = source.index("WSI capacity is a separate downstream constraint")
        dispatch_start = source.index("runtime stage=framegen-dispatch-begin")
        self.assertLess(cap_start, dispatch_start)
        self.assertIn("windowGeneratedPresentationCapDrops", source)
        self.assertIn("presentation_duty=", source)

    def test_presentation_capacity_is_target_aware_and_provisional(self) -> None:
        header = (ROOT / "include/adaptive_scheduler.hpp").read_text(
            encoding="utf-8"
        )
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        for token in (
            "GeneratedPresentationCapacityContext",
            "outputDeficit",
            "deadlineCapacityValid",
            "safeGenerationHint",
            "schedulerCostLimit",
            "sourceInsideBudget",
            "sourceDeadlineErrorNs",
            "higherCapacityProven",
            "ProfitabilityRestoreHigher",
            "TargetDeficitProbeSuccess",
        ):
            self.assertIn(token, header)

        self.assertIn(
            "GeneratedPresentationCapacityContext presentationCapacityContext",
            source,
        )
        self.assertIn(
            "generatedPresentationCapacityTracker_.limit(\n"
            "                generatedFrameCount, presentationCapacityContext)",
            source,
        )
        self.assertIn(
            "generatedPresentationCapacityTracker_.observe(\n"
            "            generatedFrameCount,\n"
            "            queuedGeneratedFrameCount,\n"
            "            generatedWsiRejectedFrameCount,\n"
            "            presentationCapacityContext)",
            source,
        )
        self.assertIn("generatedDeadlineObservationEligible", source)
        self.assertNotIn("generatedWsiObservationEligible", source)

    def test_flow_timing_is_batch_matched_and_stale_samples_are_rejected(self) -> None:
        backend = (ROOT / "framegen/public/lsfg_backend.hpp").read_text(
            encoding="utf-8"
        )
        context = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn("AdaptiveFlowBatchMetadata", backend)
        self.assertIn("sessionEpoch", backend)
        self.assertIn("batchId", backend)
        self.assertIn("predictedTotalLsfgMs", backend)
        self.assertIn("adaptiveFlowTimingEpoch_", context)
        self.assertIn("timing.batchId > this->adaptiveFlowLastObservedBatchId_", context)
        self.assertIn("timing.frameBudgetMs", context)
        self.assertIn("timing.predictedTotalLsfgMs", context)
        self.assertIn("adaptiveFlowBatchBudgetMs", context)

        # Presentation pressure can evaluate delivery capacity, but must not
        # become a source-FPS or long-term Adaptive generation backoff path.
        scheduler = (ROOT / "src/adaptive_scheduler.cpp").read_text(
            encoding="utf-8"
        )
        for rejected in (
            "sourcePreservation",
            "RaiseCausalSourceDrop",
            "source-FPS veto",
        ):
            self.assertNotIn(rejected, scheduler)

    def test_rolling_output_tracker_updates_per_completed_source_cycle(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn("lsfgOutputCadenceTracker_.observe", source)
        self.assertIn("sourceInterval, 1, this->lastGeneratedFrameCount_", source)
        self.assertIn("std::chrono::milliseconds(250), 0, 0", source)

    def test_drop_metrics_are_true_per_window_counters(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        reset_start = source.index("metrics.windowGeneratedLateDrops = 0")
        reset_end = source.index("metrics.windowCycleMs = 0.0", reset_start)
        reset = source[reset_start:reset_end]
        for field in (
            "metrics.windowGeneratedLateDrops = 0",
            "metrics.windowAdmissionRejects = 0",
            "metrics.windowGeneratedDeadlineDrops = 0",
            "metrics.windowGeneratedWsiDrops = 0",
            "metrics.windowGeneratedPresentationCapDrops = 0",
        ):
            self.assertIn(field, reset)

    def test_wsi_unavailability_does_not_poison_deadline_predictor(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        wsi_start = source.index("if (res == VK_NOT_READY || res == VK_TIMEOUT)")
        wsi_end = source.index("if (res != VK_SUCCESS", wsi_start)
        wsi_drop = source[wsi_start:wsi_end]
        self.assertIn("windowGeneratedWsiDrops", wsi_drop)
        self.assertIn("generatedWsiRejectedFrameCount", wsi_drop)
        self.assertNotIn(
            "deadlineAdmissionPredictor_.observeDeliveryMiss",
            wsi_drop,
        )
        observe_start = source.index(
            "generatedPresentationCapacityTracker_.observe("
        )
        observe_prefix = source[max(0, observe_start - 180):observe_start]
        self.assertIn("generatedDeadlineObservationEligible", observe_prefix)

        self.assertIn("generatedAcquireTimeoutNs", source)
        self.assertIn(
            "!conf.adaptiveFramegen && this->conservativeCrossDeviceSync_"
            " && !this->asyncFramegenCompletionEnabled_",
            source,
        )
        self.assertIn(
            "? runtimeWaitTimeoutNs()",
            source,
        )
        self.assertIn(
            ": 0;",
            source,
            "Adaptive and non-conservative/Xclipse generated acquires must remain nonblocking",
        )
        dispatch_start = source.index("runtime stage=framegen-dispatch-begin")
        acquire_start = source.index("ovkAcquireNextImageKHR", dispatch_start)
        self.assertGreater(acquire_start, dispatch_start)

        deadline_start = source.index("runtime stage=generated-deadline-drop")
        deadline_prefix = source[max(0, deadline_start - 900):deadline_start]
        self.assertIn(
            "deadlineAdmissionPredictor_.observeDeliveryMiss",
            deadline_prefix,
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
        context_header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
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
            "adaptive_flow_lsfg_output_valid=",
            "adaptive_flow_lsfg_output_fps=",
            "adaptive_flow_global_p95_ms=",
            "adaptive_flow_global_slow_ratio=",
            "adaptive_flow_global_pressure=",
            "adaptive_flow_compute_pressure=",
            "adaptive_flow_wsi_pressure=",
            "adaptive_flow_wsi_loss_rate=",
            "adaptive_flow_presentation_cap=",
            "adaptive_flow_presentation_duty=",
            "adaptive_flow_presentation_rejection_evidence=",
            "adaptive_flow_presentation_recovery_evidence=",
            "adaptive_flow_presentation_attempted_generated=",
            "adaptive_flow_presentation_accepted_generated=",
            "adaptive_flow_presentation_delivered_efficiency=",
            "adaptive_flow_presentation_last_change_reason=",
            "adaptive_flow_presentation_last_change_output_deficit=",
            "adaptive_flow_presentation_provisional_lower=",
            "adaptive_flow_presentation_upward_probe=",
            "adaptive_flow_output_deficit=",
            "adaptive_flow_synthetic_drop_pressure=",
            "adaptive_flow_reason=",
        )
        for field in fields:
            self.assertIn(field, source)
            self.assertIn(field, hooks)

        self.assertIn("adaptiveFlowRuntimeSnapshot()", hooks)
        for field in (
            "presentationRejectionEvidence",
            "presentationRecoveryEvidence",
            "presentationAttemptedGeneratedFrames",
            "presentationAcceptedGeneratedFrames",
            "presentationDeliveredEfficiency",
            "presentationLastChangeReason",
            "presentationLastChangeOutputDeficit",
            "presentationProvisionalLowerActive",
            "presentationUpwardProbePending",
        ):
            self.assertIn(field, context_header)


    def test_runtime_pressure_requires_a_complete_fresh_record(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        reader_start = source.index("RuntimePressureSample readRuntimePressure")
        reader_end = source.index("VkImageSubresourceRange colorSubresourceRange", reader_start)
        reader = source[reader_start:reader_end]
        for field in (
            "timestamp_ms",
            "gpu_usage_percent",
            "output_fps",
            "frame_time_p95_ms",
            "slow_frame_ratio",
        ):
            self.assertIn(f'key == "{field}"', reader)
        self.assertIn("consumed != value.size()", reader)
        self.assertIn("sawTimestamp", reader)
        self.assertIn("sawOutput", reader)
        self.assertIn("sawFrameTime", reader)
        self.assertIn("sawSlowRatio", reader)


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
            "const auto configuredPresentMode = activeConf.e_present;",
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
        desired_end = hooks.index(
            "if (state->configuredPresent != desiredPresentMode)", desired_start
        )
        desired_block = hooks[desired_start:desired_end]
        self.assertIn("desiredPresentMode = conf.e_present", desired_block)
        self.assertNotIn("adaptivePresentationPacing(conf)", desired_block)

        # Keep display-timing capability detection available, but do not let it
        # alter the restored known-good WSI pacing contract.
        self.assertIn("adaptiveDisplayTimingEnabled_ = false", context)
        self.assertIn("fifo_override=0", context)


if __name__ == "__main__":
    unittest.main()
