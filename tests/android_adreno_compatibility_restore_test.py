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

    def test_adreno_generated_cycles_use_sync_fd_input_and_completion(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        selection = source.split(
            "this->conservativeCrossDeviceSync_ =", 1
        )[1].split("std::cerr << \"lsfg-vk: Android AHB context created", 1)[0]
        self.assertIn("syncFdHandoffSupported", selection)
        self.assertIn(
            "this->conservativeCrossDeviceSync_\n"
            "            ? syncFdHandoffSupported",
            selection,
        )
        self.assertIn(
            "? VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT",
            selection,
        )
        self.assertIn("adrenoSingleQueueReadinessPoll", selection)
        self.assertIn(
            "this->asyncFramegenCompletionEnabled_ =",
            selection,
        )
        self.assertIn("syncFdHandoffSupported", selection)
        self.assertIn("this->syntheticQueue_ == VK_NULL_HANDLE", selection)
        self.assertIn(
            "this->conservativeCrossDeviceSync_\n"
            "            && info.adrenoSyntheticQueueAvailable",
            selection,
        )
        self.assertNotIn(
            "this->asyncFramegenCompletionEnabled_ =\n"
            "        !this->conservativeCrossDeviceSync_",
            selection,
        )

        handoff = source.split(
            "bool useAsyncHandoff =", 1
        )[1].split("bool asyncSubmissionIssued", 1)[0]
        self.assertIn("!conservativeSourceOnlyWarmup", handoff)
        self.assertIn("!conservativeFractionalHistoryGap", handoff)
        self.assertIn("!conservativeTrueSourceOnlyCycle", handoff)

    def test_adreno_untrained_deadline_predictor_admits_one_bootstrap_probe(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        admission_start = source.index("// Active deadline admission")
        admission_end = source.index(
            "const auto& outputCadenceForPresentation", admission_start
        )
        admission = source[admission_start:admission_end]

        self.assertIn("adrenoDeadlineBootstrapProbe", admission)
        self.assertIn("!this->deadlineAdmissionPredictor_.hasEstimate()", admission)
        self.assertIn("std::min<std::size_t>(generatedFrameCount, 1)", admission)
        self.assertIn("deadline_bootstrap_probe", source)

    def test_adreno_single_queue_polls_output_fds_before_game_queue_submission(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        completion_start = source.index(
            "if (this->asyncFramegenCompletionEnabled_\n"
            "            && framegenSync.gpuDependenciesExported)"
        )
        completion_end = source.index(
            "// 3. Compatibility/error fallback only.", completion_start
        )
        completion = source[completion_start:completion_end]

        self.assertIn("adrenoSingleQueueReadinessPoll", completion)
        self.assertIn("::poll(&outputPoll, 1, 0)", completion)
        self.assertIn("readyGeneratedFrameCount", completion)
        self.assertIn("generated-readiness-drop", completion)
        self.assertIn("conservativePendingBatchCompletePollFd_", completion)

        generated_start = source.index("// 4. Generated presentation is opportunistic.")
        generated_end = source.index("// 5. Present the real game frame", generated_start)
        generated = source[generated_start:generated_end]
        self.assertIn("if (outputReadyWaitValid.at(i))", generated)

    def test_adreno_batch_poll_blocks_ahb_reuse_without_queue_wait(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        start = source.index("bool conservativeBatchStillInFlight = false;")
        end = source.index("const bool conservativePreCopySourceBypass", start)
        poll_block = source[start:end]

        self.assertIn("conservativePendingBatchCompletePollFd_", poll_block)
        self.assertIn("::poll(&batchPoll, 1, 0)", poll_block)
        self.assertIn("conservativePendingBatchCompleteValid_ = false", poll_block)
        self.assertIn("conservativePendingBatchCompleteSemaphore_ = {}", poll_block)

    def test_adreno_completion_timeout_does_not_force_swapchain_recreation(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        start = source.index("if (!framegenReady)")
        end = source.index("if (firstPresentDiagnostic)", start)
        timeout = source[start:end]

        self.assertIn("adrenoSingleQueueReadinessPoll", timeout)
        self.assertIn('"framegen-timeout-source-only"', timeout)
        self.assertIn("conservativePendingBatchCompleteValid_ = true", timeout)

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

    def test_adreno_generated_work_uses_dedicated_same_family_queue_when_available(self) -> None:
        hooks_h = (ROOT / "include/hooks.hpp").read_text(encoding="utf-8")
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("VkQueue syntheticQueue{VK_NULL_HANDLE}", hooks_h)
        self.assertIn("adrenoSyntheticQueueAvailable", hooks_h)
        self.assertIn("augmentAdrenoSyntheticQueue", hooks)
        self.assertIn("requiresConservativeCrossDeviceSync", hooks)
        self.assertIn("queueCount >= 2", hooks)
        self.assertIn("queueCount = 2", hooks)
        self.assertIn("vkGetDeviceQueue", hooks)
        self.assertIn("adreno-synthetic-queue", hooks)

        self.assertIn("generatedWorkQueue", source)
        self.assertIn(
            "this->conservativeCrossDeviceSync_ && info.adrenoSyntheticQueueAvailable",
            source,
        )
        self.assertIn("postCopyBuf.submit(generatedWorkQueue", source)
        self.assertIn("Layer::ovkQueuePresentKHR(generatedPresentQueue", source)
        self.assertIn("armPassGpuRetirement(generatedWorkQueue)", source)

        # Non-Adreno/Xclipse continues to resolve both generated queues to the
        # existing queue handles; no capability-async/direct-storage branch is changed.
        self.assertIn(": info.queue.second;", source)
        self.assertIn(": queue;", source)

    def test_adreno_pending_batch_never_blocks_primary_source_queue(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("conservativePendingBatchCompletePollFd_", header)
        self.assertIn("poll(", source)
        self.assertIn("conservativeBatchStillInFlight", source)
        self.assertIn(
            "conservativeTrueSourceOnlyCycle || conservativeBatchStillInFlight",
            source,
        )
        self.assertIn('"compat-source-batch-inflight-bypass"', source)
        self.assertIn("::dup(batchCompleteFd)", source)

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

    def test_generated_compatibility_path_retains_bounded_completion_wait(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        generated = source.split("// 2. Tell framegen", 1)[1]
        self.assertIn(
            "bool requireHostCompletionWait = !this->asyncFramegenCompletionEnabled_",
            generated,
        )
        self.assertIn("runtimeWaitTimeoutNs()", generated)
        self.assertIn("waitContext", generated)

    def test_runtime_policy_label_matches_split_adreno_topology(self) -> None:
        policy = (ROOT / "include/android_sync_policy.hpp").read_text(encoding="utf-8")
        self.assertIn("syncfd-generated-async-adreno", policy)
        self.assertIn("capability-async", policy)

    def test_xclipse_async_path_is_not_removed(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn("syncFdHandoffSupported", source)
        self.assertIn("presentContextWithCountExportSyncFd", source)
        self.assertIn("VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT", source)


if __name__ == "__main__":
    unittest.main()
