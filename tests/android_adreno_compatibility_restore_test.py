#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidAdrenoCompatibilityRestoreTest(unittest.TestCase):
    def test_backend_exposes_driver_id_for_policy_selection(self) -> None:
        backend = (ROOT / "framegen/public/lsfg_backend.hpp").read_text(encoding="utf-8")
        device = (ROOT / "framegen/src/core/device.cpp").read_text(encoding="utf-8")
        self.assertIn("VkDriverId driverId", backend)
        self.assertIn("driverProperties.driverID", device)

    def test_context_selects_cross_device_policy_without_global_async_rollback(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn("conservativeCrossDeviceSync_", header)
        self.assertIn("selectFramegenCompatibilityPath", source)
        self.assertIn("FramegenCompatibilityPath::AdrenoLatestKnownGood", source)
        self.assertIn("backendDiagnostics.driverId", source)
        self.assertIn("sync_policy=", source)
        self.assertIn("!this->conservativeCrossDeviceSync_", source)
        self.assertIn("this->asyncAhbHandoffEnabled_", source)
        self.assertIn("this->asyncFramegenCompletionEnabled_", source)

    def test_adreno_warmup_is_not_latched_by_zero_generation_demand(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        start = source.index("const bool sourceHistoryWarmupActive")
        end = source.index("// Active deadline admission", start)
        warmup_selection = source[start:end]

        self.assertNotIn("deferConservativeWarmupUntilGenerationDemand", warmup_selection)
        self.assertIn(
            "this->requiresSourceHistoryWarmup_"
            "\n        && this->sourceHistoryWarmupRemaining_ > 0",
            warmup_selection,
        )

    def test_conservative_warmup_is_source_only_but_fractional_gaps_keep_history(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn("conservativeSourceOnlyWarmup", source)
        self.assertIn("conservativeFractionalHistoryGap", source)
        self.assertIn("conservativeTrueSourceOnlyCycle", source)
        helper = source.split("const auto presentCompatibilitySourceOnly", 1)[1].split(
            "if (conservativeSourceOnlyWarmup)", 1
        )[0]
        warmup = source.split("if (conservativeSourceOnlyWarmup)", 1)[1].split(
            "if (historyOnly)", 1
        )[0]
        self.assertIn("Layer::ovkQueuePresentKHR", helper)
        self.assertIn("presentCompatibilitySourceOnly", warmup)
        self.assertNotIn("presentContextWithCount", warmup)
        self.assertNotIn("presentContextWithCountExportSyncFd", warmup)

    def test_adreno_uses_september18_startup_and_single_reentry_warmup(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        selection = source.split(
            "this->conservativeCrossDeviceSync_ =", 1
        )[1].split('std::cerr << "lsfg-vk: Android AHB context created', 1)[0]
        self.assertIn("sourceHistoryWarmupRemaining_ = 0", selection)
        self.assertIn("requiresSourceHistoryWarmup_ = false", selection)

        bypass = source[source.index("void LsContext::enterSourceOnlyBypass()"):]
        self.assertIn(
            "this->conservativeCrossDeviceSync_ ? 1U : kSourceHistoryWarmupFrames",
            bypass,
        )


    def test_non_async_history_preprocessing_waits_before_ahb_reuse(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        history = source.split("if (historyOnly)", 1)[1].split(
            "// 2. Tell framegen", 1
        )[0]
        fallback = history.split("} else {", 1)[1]
        self.assertIn("historyRequiresHostCompletionWait = true", fallback)
        self.assertIn("waitContext", history)

    def test_adreno_uses_gpu_source_handoff_and_bounded_host_completion(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        selection_start = source.index("this->asyncAhbHandoffEnabled_ =")
        selection_end = source.index("const bool xclipseCompatibilityPath", selection_start)
        selection = source[selection_start:selection_end]

        self.assertNotIn(
            "!this->conservativeCrossDeviceSync_\n"
            "        && gameGetSemaphoreFd != nullptr",
            selection,
        )
        self.assertIn("if (this->conservativeCrossDeviceSync_)", selection)
        self.assertIn("opaqueFdHandoffSupported", selection)
        self.assertIn("VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT", selection)
        self.assertIn("gameImportSemaphoreFd != nullptr", selection)
        self.assertIn(
            "this->asyncFramegenCompletionEnabled_ =\n"
            "        !this->conservativeCrossDeviceSync_",
            selection,
        )
        self.assertIn("this->deferredAdrenoCompletionEnabled_ = false;", selection)

        generated = source.split("// 2. Tell framegen", 1)[1].split(
            "// 4. Generated presentation is opportunistic.", 1
        )[0]
        self.assertIn("bool requireHostCompletionWait =", generated)
        self.assertIn("!this->deferredAdrenoCompletionEnabled_", generated)
        self.assertIn(
            "waitContext(*this->lsfgCtxId, framegenCompletionTimeoutNs)",
            generated,
        )

    def test_adreno_untrained_predictor_uses_generic_one_frame_bootstrap(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        admission_start = source.index("// Active deadline admission")
        admission_end = source.index(
            "const auto& outputCadenceForPresentation", admission_start
        )
        admission = source[admission_start:admission_end]

        self.assertNotIn("adrenoDeadlineBootstrapProbe", admission)
        self.assertIn("!plannedBatchDecision.valid && generatedFrameCount > 1", admission)
        self.assertIn("generatedFrameCount = 1", admission)


    def test_rejected_deferred_adreno_code_is_dormant_while_host_completion_is_active(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        present = source[source.index("VkResult LsContext::present"):]
        selection_start = source.index("this->asyncAhbHandoffEnabled_ =")
        selection_end = source.index("const bool xclipseCompatibilityPath", selection_start)
        selection = source[selection_start:selection_end]

        self.assertIn("this->deferredAdrenoCompletionEnabled_ = false;", selection)
        self.assertNotIn("deferredAdrenoCompletionEnabled_ = true", source)
        # Dormant r24 lifetime code remains for auditability until device
        # qualification; no active compatibility selector can reach it.
        self.assertIn("poll(&batchPoll, 1, 0)", present)
        self.assertIn("poll(&outputPoll, 1, 0)", present)
        self.assertNotIn("adrenoSingleQueueReadinessPoll", present)
        self.assertNotIn("generated-readiness-drop", present)
        self.assertIn("waitContext(*this->lsfgCtxId, framegenCompletionTimeoutNs)", present)




    def test_adreno_bypasses_newer_pass_ring_retirement_path(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn(
            "const bool shouldRecyclePass = !this->conservativeCrossDeviceSync_;",
            source,
        )
        self.assertIn(
            "if (shouldRecyclePass && !this->tryRecyclePass(pass))",
            source,
        )
        begin = source.index("// BEGIN ADRENO_364178AF_EXECUTION")
        end = source.index("// END ADRENO_364178AF_EXECUTION", begin)
        adreno = source[begin:end]
        self.assertNotIn("tryRecyclePass", adreno)
        self.assertNotIn("submitPassCompletionFence", adreno)
        self.assertNotIn("SourceHistoryInvalidationReason::SourcePairMismatch", adreno)

    def test_adreno_host_completion_timeout_matches_september_18_recovery(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        begin = source.index("// BEGIN ADRENO_364178AF_EXECUTION")
        end = source.index("// END ADRENO_364178AF_EXECUTION", begin)
        adreno = source[begin:end]
        start = adreno.index("if (!framegenReady)")
        stop = adreno.index("metrics.windowGeneratedCompleted", start)
        timeout = adreno[start:stop]

        self.assertIn("Layer::ovkQueuePresentKHR(queue, &timeoutPresentInfo)", timeout)
        self.assertIn("VK_ERROR_OUT_OF_DATE_KHR", timeout)
        self.assertIn("pre-copy-adreno-364178af-timeout", timeout)
        self.assertNotIn("conservativePendingBatchComplete", timeout)
        self.assertNotIn("deferredAdreno", timeout)
        self.assertNotIn("sourceHistoryWarmupRemaining_", timeout)

    def test_adreno_adaptive_flow_budget_is_clamped_to_target_cadence(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        start = source.index("const double adaptiveFlowBatchBudgetMs")
        end = source.index("const auto nextAdaptiveFlowBatch", start)
        budget = source[start:end]

        self.assertIn("protectedAdrenoTargetBudgetMs", budget)
        self.assertIn("this->conservativeCrossDeviceSync_", budget)
        self.assertIn("conf.fpsLimit > 0", budget)
        self.assertIn("std::min", budget)

    def test_fixed_generated_frames_are_not_dropped_by_adaptive_deadline_policy(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        generated_start = source.index(
            "// 4. Generated presentation is opportunistic."
        )
        source_start = source.index(
            "// 5. Present the real game frame", generated_start
        )
        generated = source[generated_start:source_start]

        deadline = generated.split(
            "const uint64_t syntheticAdmissionNowNs = monotonicNowNs();", 1
        )[1].split("pass.acquireSemaphores.at(i)", 1)[0]
        self.assertIn("enforcePostDispatchSyntheticDeadline", deadline)
        self.assertIn("conf.adaptiveFramegen", deadline)
        self.assertIn("!this->conservativeCrossDeviceSync_", deadline)
        self.assertIn("this->asyncFramegenCompletionEnabled_", deadline)

    def test_adreno_async_completion_restores_post_dispatch_deadline_drop(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        generated_start = source.index(
            "// 4. Generated presentation is opportunistic."
        )
        source_start = source.index(
            "// 5. Present the real game frame", generated_start
        )
        generated = source[generated_start:source_start]

        self.assertIn("enforcePostDispatchSyntheticDeadline", generated)
        self.assertIn("conf.adaptiveFramegen", generated)
        self.assertIn(
            "!this->conservativeCrossDeviceSync_"
            " || this->asyncFramegenCompletionEnabled_",
            generated,
        )
        deadline = generated.split(
            "const uint64_t syntheticAdmissionNowNs = monotonicNowNs();", 1
        )[1].split("pass.acquireSemaphores.at(i)", 1)[0]
        self.assertIn("if (enforcePostDispatchSyntheticDeadline", deadline)

    def test_adreno_generated_acquire_uses_september18_bounded_wait_only_on_adreno(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        generated_start = source.index(
            "// 4. Generated presentation"
        )
        source_start = source.index(
            "// 5. Present the real game frame", generated_start
        )
        generated = source[generated_start:source_start]

        acquire_start = generated.index("const uint64_t generatedAcquireTimeoutNs")
        acquire_end = generated.index("auto res =", acquire_start)
        acquire = generated[acquire_start:acquire_end]
        self.assertIn("this->conservativeCrossDeviceSync_", acquire)
        self.assertIn("runtimeWaitTimeoutNs()", acquire)
        self.assertIn(": 0", acquire)

    def test_adreno_reuses_game_device_copy_command_buffers_without_changing_xclipse(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        pool_header = (ROOT / "include/mini/commandpool.hpp").read_text(encoding="utf-8")
        pool_source = (ROOT / "src/mini/commandpool.cpp").read_text(encoding="utf-8")
        command_header = (ROOT / "include/mini/commandbuffer.hpp").read_text(encoding="utf-8")
        command_source = (ROOT / "src/mini/commandbuffer.cpp").read_text(encoding="utf-8")

        self.assertIn("bool enableIndividualReset = false", pool_header)
        self.assertIn("VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT", pool_source)
        self.assertIn("void reset();", command_header)
        self.assertIn("Unable to reset command buffer", command_source)

        constructor = source[source.index("// prepare render passes"):source.index("LsContext::~LsContext()")]
        self.assertIn("this->conservativeCrossDeviceSync_", constructor)
        self.assertIn("pass.preCopyBuf = Mini::CommandBuffer", constructor)
        self.assertIn("for (auto& postCopyBuf : pass.postCopyBufs)", constructor)

        present = source[source.index("VkResult LsContext::present"):]
        self.assertIn("if (this->conservativeCrossDeviceSync_)", present)
        self.assertIn("pass.preCopyBuf.reset()", present)
        self.assertIn("postCopyBuf.reset()", present)
        self.assertIn(
            "else {\n"
            "        pass.preCopyBuf = Mini::CommandBuffer(info.device, this->cmdPool);",
            present,
        )

    def test_adreno_gpu_timing_includes_required_output_transport_copy(self) -> None:
        for relative in (
            "framegen/v3.1_src/context.cpp",
            "framegen/v3.1p_src/context.cpp",
        ):
            source = (ROOT / relative).read_text(encoding="utf-8")
            output_start = source.index("if (this->outputCopyRequired) {")
            output_end = source.index("} else {", output_start)
            output_copy = source[output_start:output_end]

            self.assertIn(
                "adaptiveFlowTimingPool->write(buf2.handle(), 3);",
                output_copy,
                "Output-copy transports must close GPU timing after transport",
            )
            copy_pos = output_copy.index("copy_same_format(")
            final_release_pos = output_copy.rindex("emit_external_barriers(")
            timing_pos = output_copy.index(
                "adaptiveFlowTimingPool->write(buf2.handle(), 3);"
            )
            self.assertGreater(timing_pos, copy_pos)
            self.assertGreater(timing_pos, final_release_pos)

            pre_output = source[max(0, output_start - 500):output_start]
            self.assertIn("!this->outputCopyRequired", pre_output)
            self.assertIn(
                "adaptiveFlowTimingPool->write(buf2.handle(), 3);",
                pre_output,
                "Direct-storage/Xclipse must keep its existing timing boundary",
            )

    def test_adreno_opaque_export_failure_uses_adreno_reprime_not_xclipse_warmup(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        failure = source.split("if (asyncExportFailed)", 1)[1].split(
            "const VkSemaphore sourceReady", 1
        )[0]
        self.assertIn("this->conservativeCrossDeviceSync_", failure)
        self.assertIn("kConservativeSourceReprimeFrames", failure)
        self.assertIn("kSourceHistoryWarmupFrames", failure)

    def test_adreno_ahb_source_slot_tracks_framegen_not_source_present_count(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("conservativeFramegenSourceIndex_", header)
        self.assertIn("const uint64_t sourceCopyIndex =", source)
        self.assertIn(
            "this->conservativeCrossDeviceSync_\n"
            "            ? this->conservativeFramegenSourceIndex_\n"
            "            : this->frameIdx",
            source,
        )
        self.assertIn("sourceCopyIndex % 2 == 0", source)
        self.assertIn("sourceCopyIndex < 2", source)
        self.assertIn("if (sourceCopyIndex == 0)", source)
        self.assertIn("++this->conservativeFramegenSourceIndex_", source)

        bypass_start = source.index("if (conservativePreCopySourceBypass)")
        bypass_end = source.index("// Android path: AHardwareBuffer exchange", bypass_start)
        bypass = source[bypass_start:bypass_end]
        self.assertNotIn("conservativeFramegenSourceIndex_++", bypass)
        self.assertNotIn("++this->conservativeFramegenSourceIndex_", bypass)

    def test_adreno_true_source_only_cycle_bypasses_private_ahb_copy(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        bypass_start = source.index("if (conservativePreCopySourceBypass)")
        private_copy = source.index(
            "pass.preCopySemaphores.at(0) = Mini::Semaphore", bypass_start
        )
        bypass = source[bypass_start:private_copy]

        self.assertIn("Layer::ovkQueuePresentKHR", bypass)
        self.assertIn("previousSourceCopySignalValid_ = false", bypass)
        self.assertIn("kConservativeSourceReprimeFrames - 1", bypass)
        self.assertNotIn("copySwapchainToExternalAhb", bypass)
        self.assertNotIn("submitAndWaitForAhbHandoff", bypass)

    def test_adreno_warmup_and_zero_history_use_host_fence_copy(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        handoff_start = source.index("const auto handoffStart")
        history_start = source.index("if (historyOnly)", handoff_start)
        handoff = source[handoff_start:history_start]

        self.assertIn("conservativeSourceOnlyWarmup", handoff)
        self.assertIn("submitAndWaitForAhbHandoff", handoff)
        self.assertNotIn("conservativeCopyOnlyHistory", handoff)
        self.assertIn("\"host-fence\"", source)

    def test_adreno_fractional_gap_advances_zero_count_history_with_host_fence(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        history_start = source.index("if (historyOnly)")
        generation_start = source.index(
            "// 2. Tell framegen to generate intermediary frames.", history_start
        )
        history = source[history_start:generation_start]

        self.assertIn("conservativeHistoryGap", source)
        self.assertIn("generatedFrameCount == 0", source)
        self.assertIn("presentContextWithCount(", history)
        self.assertIn("historyRequiresHostCompletionWait = true", history)
        self.assertNotIn("if (conservativeAdaptiveHistoryGap)", source)
        self.assertNotIn("compat-adaptive-history-copy", source)

    def test_handoff_metrics_separate_submit_wait_and_cross_frame_dependencies(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        for token in (
            "windowHandoffSubmitMs",
            "windowHandoffFenceWaitMs",
            "windowHandoffPrevSourceDeps",
            "windowHandoffBatchDeps",
        ):
            self.assertIn(token, header)
        for token in (
            "ahb_submit_avg_ms=",
            "ahb_host_wait_avg_ms=",
            "ahb_prev_source_deps=",
            "ahb_batch_deps=",
        ):
            self.assertIn(token, source)

    def test_xclipse_async_completion_gate_is_unchanged(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        selection_start = source.index("this->asyncFramegenCompletionEnabled_ =")
        selection_end = source.index(
            "const bool xclipseCompatibilityPath", selection_start
        )
        selection = source[selection_start:selection_end]

        self.assertIn(
            "!this->conservativeCrossDeviceSync_",
            selection,
            "non-conservative/Xclipse must retain capability-driven async completion",
        )
        self.assertIn("syncFdHandoffSupported", selection)
        self.assertIn("gameImportSemaphoreFd != nullptr", selection)

    def test_adreno_zero_count_history_is_not_an_off_transition(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        history_start = source.index("if (historyOnly)")
        generation_start = source.index(
            "// 2. Tell framegen to generate intermediary frames.", history_start
        )
        history = source[history_start:generation_start]

        self.assertIn("presentContextWithCount(", history)
        self.assertIn("historyRequiresHostCompletionWait = true", history)
        self.assertNotIn("compat-adaptive-history-copy", source)
        self.assertNotIn("if (conservativeAdaptiveHistoryGap)", source)

    def test_adreno_syncfd_import_failure_transfers_fd_ownership_once(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        generated = source.split(
            "if (this->asyncFramegenCompletionEnabled_\n"
            "            && framegenSync.gpuDependenciesExported)", 1
        )[1].split("// 3. Compatibility/error fallback only.", 1)[0]

        self.assertIn("const int fd = framegenSync.outputReadyFds.at(i);", generated)
        self.assertIn(
            "if (this->conservativeCrossDeviceSync_)\n"
            "                        framegenSync.outputReadyFds.at(i) = -1;",
            generated,
        )
        self.assertIn(
            "if (!this->conservativeCrossDeviceSync_)\n"
            "                        framegenSync.outputReadyFds.at(i) = -1;",
            generated,
        )
        self.assertIn(
            "const int batchCompleteFd = framegenSync.batchCompleteFd;",
            generated,
        )
        self.assertIn(
            "if (this->conservativeCrossDeviceSync_)\n"
            "                        framegenSync.batchCompleteFd = -1;",
            generated,
        )
        self.assertIn(
            "if (!this->conservativeCrossDeviceSync_)\n"
            "                        framegenSync.batchCompleteFd = -1;",
            generated,
        )

    def test_adreno_does_not_rewrite_game_device_queue_creation(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        self.assertNotIn("AdrenoSyntheticQueuePlan", hooks)
        self.assertNotIn("augmentAdrenoSyntheticQueue", hooks)
        self.assertNotIn("kAdrenoSyntheticQueuePriority", hooks)
        self.assertNotIn("queueInfo.queueCount = plan.queueIndex + 1", hooks)
        self.assertNotIn("adreno-synthetic-queue", hooks)


    def test_adreno_device_info_leaves_synthetic_queue_unset(self) -> None:
        hooks_h = (ROOT / "include/hooks.hpp").read_text(encoding="utf-8")
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        self.assertIn("VkQueue syntheticQueue{VK_NULL_HANDLE}", hooks_h)
        self.assertIn("adrenoSyntheticQueueAvailable{false}", hooks_h)
        self.assertNotIn(".syntheticQueue =", hooks)
        self.assertNotIn(".adrenoSyntheticQueueAvailable =", hooks)


    def test_adreno_driver_detection_remains_in_sync_policy_not_queue_creation(self) -> None:
        policy = (ROOT / "include/android_sync_policy.hpp").read_text(encoding="utf-8")
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        self.assertIn("requiresConservativeCrossDeviceSync", policy)
        self.assertNotIn("requiresConservativeCrossDeviceSync", hooks)


    def test_adreno_deferred_export_failure_keeps_bounded_host_fallback(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        generated = source.split("// 2. Tell framegen", 1)[1]
        self.assertIn("deferredAdrenoCompletionEnabled_", generated)
        self.assertIn("framegenSync.hostWaitFallback", generated)
        self.assertIn("requireHostCompletionWait = true", generated)
        self.assertIn("runtimeWaitTimeoutNs()", generated)
        self.assertIn("waitContext", generated)
        self.assertIn("generatedWorkQueue", source)
        self.assertIn(": info.queue.second;", source)

    def test_deferred_adreno_state_is_separate_from_abandoned_pending_source_stack(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn("deferredAdrenoBatchValid_", header)
        self.assertIn("deferredAdrenoOutputReadyFds_", header)
        self.assertIn("deferredAdrenoBatchCompleteFd_", header)
        self.assertIn("runtime stage=adreno-deferred-batch-queued", source)
        self.assertNotIn("adrenoSingleQueueReadinessPoll", source)
        self.assertNotIn("generated-readiness-drop", source)

    def test_adreno_surface_loss_is_propagated_without_degraded_translation(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn("isAdrenoWsiRetirementResult", source)

        helper = source.split("const auto presentCompatibilitySourceOnly", 1)[1].split(
            "if (conservativeAdaptiveHistoryGap)", 1
        )[0]
        self.assertIn(
            "this->conservativeCrossDeviceSync_"
            " && isAdrenoWsiRetirementResult(sourceResult)",
            helper,
        )
        bypass = source.split("if (conservativePreCopySourceBypass)", 1)[1].split(
            "// Android path:", 1
        )[0]
        self.assertIn("isAdrenoWsiRetirementResult(bypassResult)", bypass)

    def test_generated_compatibility_path_retains_bounded_completion_fallback(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        generated = source.split("// 2. Tell framegen", 1)[1]
        self.assertIn("bool requireHostCompletionWait =", generated)
        self.assertIn("!this->deferredAdrenoCompletionEnabled_", generated)
        self.assertIn("requireHostCompletionWait = true", generated)
        self.assertIn("runtimeWaitTimeoutNs()", generated)
        self.assertIn("waitContext", generated)

    def test_runtime_policy_label_reports_source_protection_not_input_transport(self) -> None:
        policy = (ROOT / "include/android_sync_policy.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn("adreno-source-protected-host-completion", policy)
        self.assertNotIn("opaque-fd-input-host-completion-adreno", policy)
        self.assertIn("capability-async", policy)
        self.assertIn("handoff=", source)
        self.assertIn("completion=", source)

    def test_xclipse_async_path_is_not_removed(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn("syncFdHandoffSupported", source)
        self.assertIn("presentContextWithCountExportSyncFd", source)
        self.assertIn("VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT", source)


if __name__ == "__main__":
    unittest.main()
