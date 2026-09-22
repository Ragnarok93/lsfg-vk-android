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
        self.assertIn("requiresConservativeCrossDeviceSync", source)
        self.assertIn("backendDiagnostics.driverId", source)
        self.assertIn("sync_policy=", source)
        self.assertIn("!this->conservativeCrossDeviceSync_", source)
        self.assertIn("this->asyncAhbHandoffEnabled_", source)
        self.assertIn("this->asyncFramegenCompletionEnabled_", source)

    def test_adreno_zero_demand_defers_reprime_until_generation_is_requested(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        start = source.index("const bool deferConservativeWarmupUntilGenerationDemand")
        end = source.index("// Active deadline admission", start)
        warmup_selection = source[start:end]

        self.assertIn("this->conservativeCrossDeviceSync_", warmup_selection)
        self.assertIn("conf.adaptiveFramegen", warmup_selection)
        self.assertIn("plannedGeneratedFrameCount == 0", warmup_selection)
        self.assertIn(
            "&& !deferConservativeWarmupUntilGenerationDemand",
            warmup_selection,
        )

        warmup = source.split("if (conservativeSourceOnlyWarmup)", 1)[1].split(
            "if (historyOnly)", 1
        )[0]
        self.assertIn("--this->sourceHistoryWarmupRemaining_", warmup)
        self.assertIn("presentCompatibilitySourceOnly", warmup)

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

    def test_adreno_uses_parity_safe_source_reprime_without_startup_delay(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn("kConservativeSourceReprimeFrames = 2", source)
        self.assertIn("kConservativeSourceReprimeFrames - 1", source)
        self.assertIn(
            "this->conservativeCrossDeviceSync_ ? kConservativeSourceReprimeFrames",
            source,
        )
        selection = source.split(
            "this->conservativeCrossDeviceSync_ =", 1
        )[1].split("std::cerr << \"lsfg-vk: Android AHB context created", 1)[0]
        self.assertIn("sourceHistoryWarmupRemaining_ = 0", selection)
        self.assertIn("requiresSourceHistoryWarmup_ = false", selection)

    def test_non_async_history_preprocessing_waits_before_ahb_reuse(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        history = source.split("if (historyOnly)", 1)[1].split(
            "// 2. Tell framegen", 1
        )[0]
        fallback = history.split("} else {", 1)[1]
        self.assertIn("historyRequiresHostCompletionWait = true", fallback)
        self.assertIn("waitContext", history)

    def test_single_queue_adreno_restores_device_proven_host_completion(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        selection_start = source.index("this->asyncAhbHandoffEnabled_ =")
        selection_end = source.index("// The known-good Qualcomm/Adreno path", selection_start)
        selection = source[selection_start:selection_end]

        self.assertIn(
            "this->asyncAhbHandoffEnabled_ =\n"
            "        !this->conservativeCrossDeviceSync_",
            selection,
            "Adreno must not export source readiness into the private framegen device",
        )
        self.assertIn(
            "this->conservativeCrossDeviceSync_\n"
            "            ? VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT",
            selection,
            "Adreno's -1 input fd must remain an absent input semaphore, not SYNC_FD -1",
        )
        self.assertIn(
            "this->asyncFramegenCompletionEnabled_ =\n"
            "        !this->conservativeCrossDeviceSync_",
            selection,
            "Adreno must retain bounded private-device completion before output AHB readback",
        )
        self.assertIn("this->deferredAdrenoCompletionEnabled_ = false;", selection)
        self.assertNotIn(
            "this->syntheticQueue_ != VK_NULL_HANDLE",
            selection,
            "a hypothetical synthetic queue must not reopen an unproven Adreno completion path",
        )
        self.assertIn("this->syntheticQueue_ = VK_NULL_HANDLE;", source)

        handoff_start = source.index("// Xclipse/generic capability paths may hand source readiness")
        handoff_end = source.index("if (useAsyncHandoff)", handoff_start)
        handoff = source[handoff_start:handoff_end]
        self.assertIn(
            "bool useAsyncHandoff =\n"
            "        !this->conservativeCrossDeviceSync_\n"
            "        && this->asyncAhbHandoffEnabled_;",
            handoff,
        )
        self.assertIn("int framegenInputSemaphoreFd = -1;", handoff)

        generated = source.split("// 2. Tell framegen", 1)[1].split(
            "// 4. Generated presentation is opportunistic.", 1
        )[0]
        self.assertIn("bool requireHostCompletionWait =", generated)
        self.assertIn("waitContext(*this->lsfgCtxId, framegenCompletionTimeoutNs)", generated)

        generated_present = source.index("// 4. Generated presentation is opportunistic.")
        source_present = source.index("// 5. Present the real game frame", generated_present)
        self.assertLess(
            generated_present,
            source_present,
            "Adreno must keep generated-then-source delivery inside the same intercepted call",
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


    def test_adreno_single_queue_never_zero_time_polls_generated_outputs(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        present = source[source.index("VkResult LsContext::present"):]
        self.assertNotIn("adrenoSingleQueueReadinessPoll", present)
        self.assertNotIn("generated-readiness-drop", present)
        self.assertNotIn("readyGeneratedFrameCount", present)
        self.assertIn("waitContext(*this->lsfgCtxId, framegenCompletionTimeoutNs)", present)




    def test_adreno_pass_ring_pressure_does_not_restart_reprime_epoch(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        start = source.index("if (!this->tryRecyclePass(pass))")
        end = source.index("#ifdef __ANDROID__", start + 1)
        # Include the Android-specific branch immediately following the
        # successful passthrough present.
        end = source.index("#endif", end) + len("#endif")
        pressure = source[start:end]

        self.assertIn("this->conservativeCrossDeviceSync_", pressure)
        self.assertIn(
            "kConservativeSourceReprimeFrames - 1",
            pressure,
            "Adreno pass pressure should request only the one copy needed to reprime",
        )
        self.assertIn("previousSourceCopySignalValid_ = false", pressure)
        self.assertIn("lastDispatchedGeneratedFrameCount_ = 0", pressure)
        self.assertIn(
            "else {\n                this->resetAdaptiveSourceEpoch(true);",
            pressure,
            "non-Adreno/Xclipse behavior must keep the existing epoch reset",
        )

        adreno_branch = pressure.split(
            "if (this->conservativeCrossDeviceSync_)", 1
        )[1].split("} else {", 1)[0]
        self.assertNotIn("resetAdaptiveSourceEpoch", adreno_branch)
        self.assertNotIn("sourceTimeline_.reset", adreno_branch)
        self.assertNotIn("adaptiveScheduler_.reset", adreno_branch)

    def test_adreno_host_completion_timeout_uses_normal_recovery_not_history_loop(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        start = source.index("if (!framegenReady)")
        end = source.index("if (firstPresentDiagnostic)", start)
        timeout = source[start:end]

        self.assertIn("adrenoHostCompletionFallback", timeout)
        self.assertIn("conservativePendingBatchCompleteValid_ = false", timeout)
        self.assertIn('"pre-copy-timeout"', timeout)
        self.assertNotIn('"framegen-timeout-source-only"', timeout)
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

    def test_adreno_async_completion_keeps_fixed_generated_acquire_nonblocking(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        generated_start = source.index(
            "// 4. Generated presentation is opportunistic."
        )
        source_start = source.index(
            "// 5. Present the real game frame", generated_start
        )
        generated = source[generated_start:source_start]

        self.assertIn("generatedAcquireTimeoutNs", generated)
        self.assertIn(
            "!conf.adaptiveFramegen && this->conservativeCrossDeviceSync_"
            " && !this->asyncFramegenCompletionEnabled_",
            generated,
        )
        self.assertIn("runtimeWaitTimeoutNs()", generated)
        self.assertIn(": 0;", generated)

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

    def test_adreno_reprime_copy_is_queued_without_host_fence_wait(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        handoff_start = source.index("const auto handoffStart")
        host_fallback = source.index(
            "submitAndWaitForAhbHandoff", handoff_start
        )
        handoff = source[handoff_start:host_fallback]

        self.assertIn("conservativeSourceOnlyWarmup", handoff)
        self.assertIn("submitAhbHandoff", handoff)
        self.assertIn("queuedCopyWithoutHostWait", handoff)

    def test_adreno_fractional_gap_queues_copy_without_zero_count_framegen(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        handoff_start = source.index("const auto handoffStart")
        history_start = source.index("if (historyOnly)", handoff_start)
        pre_history = source[handoff_start:history_start]

        self.assertIn("conservativeFractionalHistoryGap", pre_history)
        self.assertIn("queuedCopyWithoutHostWait", pre_history)
        self.assertIn(
            'presentCompatibilitySourceOnly(\n'
            '            "compat-fractional-history-copy"',
            pre_history,
        )
        fractional = pre_history.split(
            "if (conservativeFractionalHistoryGap)", 1
        )[1]
        self.assertNotIn("presentContextWithCount(", fractional)
        self.assertNotIn("presentContextWithCountExportSyncFd(", fractional)

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
        selection_end = source.index("// The known-good Qualcomm/Adreno path", selection_start)
        selection = source[selection_start:selection_end]

        self.assertIn(
            "!this->conservativeCrossDeviceSync_",
            selection,
            "non-conservative/Xclipse must retain capability-driven async completion",
        )
        self.assertIn("syncFdHandoffSupported", selection)
        self.assertIn("gameImportSemaphoreFd != nullptr", selection)

    def test_adreno_async_completion_does_not_reopen_zero_count_framegen(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        fractional = source.index("if (conservativeFractionalHistoryGap)")
        warmup = source.index("if (conservativeSourceOnlyWarmup)", fractional)
        history = source.index("if (historyOnly)", warmup)

        self.assertLess(fractional, history)
        self.assertLess(warmup, history)
        pre_history = source[fractional:history]
        self.assertIn("presentCompatibilitySourceOnly", pre_history)
        self.assertNotIn("presentContextWithCountExportSyncFd", pre_history)
        self.assertNotIn("presentContextWithCount(", pre_history)

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
            "if (conservativeFractionalHistoryGap)", 1
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

    def test_runtime_policy_label_matches_split_adreno_topology(self) -> None:
        policy = (ROOT / "include/android_sync_policy.hpp").read_text(encoding="utf-8")
        self.assertIn("host-fence-input-host-completion-adreno", policy)
        self.assertIn("capability-async", policy)

    def test_xclipse_async_path_is_not_removed(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn("syncFdHandoffSupported", source)
        self.assertIn("presentContextWithCountExportSyncFd", source)
        self.assertIn("VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT", source)


if __name__ == "__main__":
    unittest.main()
