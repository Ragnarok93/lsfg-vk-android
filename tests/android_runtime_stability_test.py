#!/usr/bin/env python3
import unittest
from pathlib import Path

from android_lifecycle_regression_test import AndroidLifecycleRegressionTest  # noqa: F401

ROOT = Path(__file__).resolve().parents[1]


class AndroidRuntimeStabilityContractTest(unittest.TestCase):
    def test_transient_command_pool_is_preserved(self) -> None:
        source = (ROOT / "src/mini/commandpool.cpp").read_text(encoding="utf-8")
        self.assertIn("VK_COMMAND_POOL_CREATE_TRANSIENT_BIT", source)

    def test_per_frame_handle_owners_use_single_allocation(self) -> None:
        semaphore = (ROOT / "src/mini/semaphore.cpp").read_text(encoding="utf-8")
        command_buffer = (ROOT / "src/mini/commandbuffer.cpp").read_text(encoding="utf-8")
        self.assertIn("std::make_shared<SemaphoreOwner>", semaphore)
        self.assertIn("std::shared_ptr<VkSemaphore>(owner, &owner->handle)", semaphore)
        self.assertNotIn("new VkSemaphore(", semaphore)
        self.assertIn("std::make_shared<CommandBufferOwner>", command_buffer)
        self.assertIn("std::shared_ptr<VkCommandBuffer>(owner, &owner->handle)", command_buffer)
        self.assertNotIn("new VkCommandBuffer(", command_buffer)

    def test_framegen_wrappers_preserve_dependency_destruction_order(self) -> None:
        pipeline_header = (ROOT / "framegen/include/core/pipeline.hpp").read_text(
            encoding="utf-8"
        )
        pipeline_source = (ROOT / "framegen/src/core/pipeline.cpp").read_text(
            encoding="utf-8"
        )
        descriptor_header = (ROOT / "framegen/include/core/descriptorset.hpp").read_text(
            encoding="utf-8"
        )
        descriptor_source = (ROOT / "framegen/src/core/descriptorset.cpp").read_text(
            encoding="utf-8"
        )

        self.assertLess(
            pipeline_header.index("std::shared_ptr<VkPipelineLayout> layout"),
            pipeline_header.index("std::shared_ptr<VkPipeline> pipeline"),
            "Pipeline must be destroyed before its layout because members are destroyed in reverse declaration order",
        )
        self.assertIn("PipelineHandleGuard", pipeline_source)
        self.assertIn("~DescriptorSetUpdateBuilder() noexcept", descriptor_header)
        self.assertIn("this->clearEntries()", descriptor_source)

    def test_common_submit_path_keeps_stage_masks_off_heap(self) -> None:
        source = (ROOT / "src/mini/commandbuffer.cpp").read_text(encoding="utf-8")
        self.assertIn("std::array<VkPipelineStageFlags, 4> inlineWaitStages", source)
        self.assertIn("waitSemaphores.size() <= inlineWaitStages.size()", source)

    def test_android_handoff_reuses_one_context_fence_and_bounds_wait(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        submit_start = source.index("void submitAhbHandoff(")
        wait_start = source.index("void waitForAhbHandoff(", submit_start)
        fallback_start = source.index("void submitAndWaitForAhbHandoff(", wait_start)
        namespace_end = source.index("} // namespace", fallback_start)
        submit_helper = source[submit_start:wait_start]
        wait_helper = source[wait_start:fallback_start]
        fallback_helper = source[fallback_start:namespace_end]

        self.assertIn("std::shared_ptr<VkFence> ahbHandoffFence", header)
        self.assertIn("PFN_vkResetFences resetHandoffFences", header)
        self.assertIn("PFN_vkWaitForFences waitHandoffFences", header)
        self.assertIn("resetFences(device, 1, &fence)", submit_helper)
        self.assertIn("commandBuffer.submit", submit_helper)
        self.assertIn("waitForFences(", wait_helper)
        self.assertIn("runtimeWaitTimeoutNs()", wait_helper)
        self.assertNotIn("UINT64_MAX", wait_helper)
        self.assertIn("submitAhbHandoff(", fallback_helper)
        self.assertIn("waitForAhbHandoff(device, fence, waitForFences)", fallback_helper)
        self.assertNotIn('"vkCreateFence"', submit_helper + wait_helper + fallback_helper)
        self.assertNotIn('"vkDestroyFence"', submit_helper + wait_helper + fallback_helper)

    def test_pass_ring_retires_gpu_resources_before_reuse(self) -> None:
        """Regression: a slow WSI/translation backlog must not destroy an in-flight pass slot."""
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("completionFence", header)
        self.assertIn("completionFenceSubmitted", header)
        self.assertIn("tryRecyclePass", source)
        self.assertIn("submitPassCompletionFence", source)
        self.assertIn("VK_TRUE, 0", source)
        self.assertIn("commandBufferCount = 0", source)
        self.assertIn("completionFenceSubmitted = true", source)

    def test_context_teardown_waits_the_game_queue_before_freeing_resources(self) -> None:
        """Regression: swapchain retirement must not free pass handles under GPU use."""
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        destructor_start = source.index("LsContext::~LsContext()")
        present_start = source.index("VkResult LsContext::present", destructor_start)
        destructor = source[destructor_start:present_start]

        self.assertIn("queueWaitIdle", destructor)
        self.assertIn("waitQueueIdle_", destructor)
        self.assertLess(
            destructor.index("waitQueueIdle_"),
            destructor.index("lsfgCtxId.reset()"),
        )

    def test_present_wait_semaphore_lifetime_is_tied_to_swapchain_image_reacquire(self) -> None:
        """Present wait semaphores must outlive WSI use, not merely a later queue submit fence."""
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("presentWaitRetirements_", header)
        self.assertIn("releasePresentWaitRetirements", source)
        self.assertIn("retainPresentWait", source)
        self.assertIn("releasePresentWaitRetirements(presentIdx)", source)

        acquire = source.index("Layer::ovkAcquireNextImageKHR")
        release = source.index("releasePresentWaitRetirements(imageIdx)", acquire)
        self.assertGreater(release, acquire)

        finish_start = source.index("const auto finishSourcePresent")
        finish_end = source.index("// Android path:", finish_start)
        finish = source[finish_start:finish_end]
        self.assertNotIn(
            "submitPassCompletionFence(pass, queue)",
            finish,
            "A fence submitted after vkQueuePresentKHR cannot prove present-wait lifetime.",
        )

    def test_cross_frame_wait_semaphores_are_owned_by_the_consuming_pass(self) -> None:
        """Producer pass reuse cannot destroy semaphores still queued as next-pass waits."""
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("crossFrameWaitRetentions", header)
        self.assertIn(
            "pass.crossFrameWaitRetentions.emplace_back(",
            source,
        )
        recycle_start = source.index("bool LsContext::tryRecyclePass")
        recycle_end = source.index("bool LsContext::submitPassCompletionFence", recycle_start)
        recycle = source[recycle_start:recycle_end]
        self.assertIn("pass.crossFrameWaitRetentions.clear()", recycle)

    def test_pass_ring_backpressure_is_non_adreno_only(self) -> None:
        """The September 18 Adreno island must not enter newer pass-ring retirement fallback."""
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        guard = (
            "if (!this->conservativeCrossDeviceSync_ "
            "&& !this->tryRecyclePass(pass))"
        )
        self.assertIn(guard, source)

        begin = source.index("// BEGIN ADRENO_364178AF_EXECUTION")
        end = source.index("// END ADRENO_364178AF_EXECUTION", begin)
        adreno = source[begin:end]
        self.assertNotIn("tryRecyclePass", adreno)
        self.assertNotIn("SourceHistoryInvalidationReason::SourcePairMismatch", adreno)

    def test_adreno_standalone_reprime_preserves_two_input_parity(self) -> None:
        """Standalone Adreno reset needs two source copies; zero-cycle recovery needs one more."""
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("kConservativeSourceReprimeFrames = 2", source)
        self.assertIn(
            "kConservativeSourceReprimeFrames - 1",
            source,
            "A true source-only cycle already contributed the first of the two parity copies.",
        )

    def test_source_timeline_discontinuity_resets_all_adaptive_epoch_state(self) -> None:
        """Regression: source-only discontinuities cannot leave scheduler/capacity history armed."""
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("resetAdaptiveSourceEpoch", header)
        self.assertIn("resetAdaptiveSourceEpoch", source)
        self.assertIn("adaptiveScheduler_.reset()", source)
        self.assertIn("generatedPresentationCapacityTracker_.reset()", source)
        self.assertIn("lsfgOutputCadenceTracker_.reset()", source)
        self.assertIn("sourceTimeline_.reset()", source)

    def test_present_hook_debounces_fs_and_reuses_wait_storage(self) -> None:
        source = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        self.assertIn("Clock::time_point nextConfigPoll", source)
        self.assertIn("std::chrono::milliseconds(250)", source)
        self.assertIn("std::vector<VkSemaphore> presentWaitSemaphores", source)
        self.assertIn("auto& semaphores = runtimeStats.presentWaitSemaphores", source)

    def test_cross_device_framegen_completion_is_bounded_without_device_wait_idle(self) -> None:
        wrapper = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        present_start = wrapper.index("VkResult LsContext::present")
        desktop_start = wrapper.index(
            "// Desktop Linux path: OPAQUE_FD semaphore-based synchronization",
            present_start,
        )
        android_present = wrapper[present_start:desktop_start]
        self.assertIn("submitAndWaitForAhbHandoff", android_present)
        self.assertIn("waitContext", android_present)
        self.assertIn("runtimeWaitTimeoutNs()", android_present)
        self.assertNotIn("LSFG_3_1P::waitIdle();", android_present)
        self.assertNotIn("LSFG_3_1::waitIdle();", android_present)

        for relative in (
            "framegen/v3.1_src/context.cpp",
            "framegen/v3.1p_src/context.cpp",
        ):
            context_source = (ROOT / relative).read_text(encoding="utf-8")
            self.assertIn("framegenWaitTimeoutNs()", context_source)
            self.assertIn("bool Context::waitForCompletion", context_source)
            self.assertNotIn("UINT64_MAX", context_source)

        for relative in (
            "framegen/v3.1_src/lsfg.cpp",
            "framegen/v3.1p_src/lsfg.cpp",
        ):
            lifecycle_source = (ROOT / relative).read_text(encoding="utf-8")
            self.assertIn("waitForCompletion(*device)", lifecycle_source)
            self.assertNotIn(
                "vkDeviceWaitIdle",
                lifecycle_source,
                "Framegen completion and teardown must use bounded context fences rather than an uninterruptible device-wide idle wait",
            )

    def test_recursive_layer_disable_is_exception_safe_and_serialized(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn("std::mutex lsfgDisableEnvMutex", source)
        self.assertIn("class ScopedLsfgDisable", source)
        self.assertIn("previousValue_", source)
        self.assertIn("ScopedLsfgDisable disableRecursiveInterception", source)
        self.assertEqual(source.count('unsetenv("DISABLE_LSFG")'), 1)

    def test_generated_wsi_policy_is_historical_on_adreno_and_opportunistic_elsewhere(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        present_start = source.index("VkResult LsContext::present")
        desktop_start = source.index(
            "// Desktop Linux path: OPAQUE_FD semaphore-based synchronization",
            present_start,
        )
        android_present = source[present_start:desktop_start]
        generated_start = android_present.index("// 4. Generated presentation")
        source_start = android_present.index(
            "// 5. Present the real game frame", generated_start
        )
        generated = android_present[generated_start:source_start]

        acquire_start = generated.index("const uint64_t generatedAcquireTimeoutNs")
        acquire_end = generated.index("auto res =", acquire_start)
        acquire = generated[acquire_start:acquire_end]
        self.assertIn("this->conservativeCrossDeviceSync_", acquire)
        self.assertIn("runtimeWaitTimeoutNs()", acquire)
        self.assertIn(": 0", acquire)

        # Newer deadline/capacity policy remains available for generic/Xclipse,
        # but the protected Adreno WSI transaction is not converted into a
        # zero-time opportunistic acquire.
        self.assertIn("syntheticAdmissionNowNs >= syntheticDesiredTimeNs", generated)
        self.assertIn("stage=generated-deadline-drop", generated)
        self.assertIn(
            "!this->conservativeCrossDeviceSync_\n"
            "                && (res == VK_NOT_READY || res == VK_TIMEOUT)",
            generated,
        )
        self.assertIn("droppedGeneratedFrames = generatedFrameCount - i", generated)
        self.assertIn("metrics.windowGeneratedLateDrops", generated)

        # Preserve September 18 application pNext ownership on Adreno while
        # generic/Xclipse keeps the newer source-owned chain.
        self.assertIn("generatedDownstreamPNext", generated)
        self.assertIn(
            "this->conservativeCrossDeviceSync_ && i == 0",
            generated,
        )

        source_tail = android_present[source_start:]
        self.assertIn(
            "lastPrevPostCopySemaphore = queuedGeneratedFrameCount > 0",
            source_tail,
        )
        self.assertIn("finalSourceDownstreamPNext", source_tail)
        self.assertIn(
            "this->conservativeCrossDeviceSync_\n"
            "            ? (queuedGeneratedFrameCount == 0 ? pNext : nullptr)",
            source_tail,
        )
        self.assertIn(
            "this->lastGeneratedFrameCount_ = queuedGeneratedFrameCount",
            android_present,
        )


    def test_adreno_async_completion_enforces_post_dispatch_deadline(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        generated_start = source.index("// 4. Generated presentation is opportunistic.")
        source_start = source.index("// 5. Present the real game frame", generated_start)
        generated = source[generated_start:source_start]

        self.assertIn(
            "!this->conservativeCrossDeviceSync_"
            " || this->asyncFramegenCompletionEnabled_",
            generated,
        )

    def test_adreno_game_copy_command_buffers_are_reused(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        pool_header = (ROOT / "include/mini/commandpool.hpp").read_text(encoding="utf-8")
        pool_source = (ROOT / "src/mini/commandpool.cpp").read_text(encoding="utf-8")
        command_header = (ROOT / "include/mini/commandbuffer.hpp").read_text(encoding="utf-8")

        self.assertIn("bool enableIndividualReset = false", pool_header)
        self.assertIn("VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT", pool_source)
        self.assertIn("void reset();", command_header)
        self.assertIn("pass.preCopyBuf.reset()", source)
        self.assertIn("postCopyBuf.reset()", source)

    def test_adreno_execution_island_excludes_deferred_batch_architecture(self) -> None:
        """Deferred batches remain dormant and cannot affect the protected Adreno path."""
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")

        # Dormant state may remain for audit/non-selected experiments.
        self.assertIn("deferredAdrenoBatchValid_", header)

        begin = source.index("// BEGIN ADRENO_364178AF_EXECUTION")
        end = source.index("// END ADRENO_364178AF_EXECUTION", begin)
        adreno = source[begin:end]
        self.assertNotIn("deferredAdreno", adreno)
        self.assertNotIn("conservativePendingBatchComplete", adreno)
        self.assertNotIn("conservativePendingHistoryComplete", adreno)
        self.assertNotIn("crossFrameWaitRetentions", adreno)

    def test_adreno_source_pair_uses_september_18_frame_index_parity(self) -> None:
        """Protected Adreno uses wrapper frameIdx parity exactly as 364178af."""
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        begin = source.index("// BEGIN ADRENO_364178AF_EXECUTION")
        end = source.index("// END ADRENO_364178AF_EXECUTION", begin)
        adreno = source[begin:end]

        self.assertIn("this->frameIdx % 2 == 0", adreno)
        self.assertIn("this->frameIdx < 2", adreno)
        self.assertNotIn("conservativeFramegenSourceIndex_", adreno)
        self.assertNotIn("sourceCopyIndex", adreno)
        self.assertNotIn("if (sourceCopyIndex == 0)", adreno)

    def test_fixed_and_adaptive_discontinuities_rebuild_history(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn("sourceTimelineDiscontinuity", source)
        self.assertIn("hadValidSourceTimeline", source)
        self.assertIn(
            "hadValidSourceTimeline\n"
            "                && !this->currentSourceTimeline_.valid",
            source,
        )
        # Non-Adreno drivers retain the full four-cycle private-history flush,
        # while the serialized Qualcomm/Adreno path uses a single protected
        # source reprime. Both reset paths must remain present.
        self.assertGreaterEqual(source.count("kSourceHistoryWarmupFrames"), 4)
        self.assertGreaterEqual(
            source.count("kConservativeSourceReprimeFrames"),
            3,
        )
        self.assertIn("this->conservativeCrossDeviceSync_", source)
        self.assertGreaterEqual(
            source.count("deadlineAdmissionPredictor_.reset()"),
            2,
        )

    def test_zero_generation_history_uses_async_dependency_chain(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        present_start = source.index("VkResult LsContext::present")
        desktop_start = source.index(
            "// Desktop Linux path: OPAQUE_FD semaphore-based synchronization",
            present_start,
        )
        android_present = source[present_start:desktop_start]

        self.assertIn("bool useAsyncHandoff =", android_present)
        self.assertIn("this->asyncAhbHandoffEnabled_", android_present)
        self.assertIn("!conservativeSourceOnlyWarmup", android_present)
        self.assertIn("!conservativeTrueSourceOnlyCycle", android_present)
        history_start = android_present.index("if (historyOnly)")
        generation_start = android_present.index(
            "// 2. Tell framegen to generate intermediary frames.", history_start
        )
        history = android_present[history_start:generation_start]
        self.assertIn("presentContextWithCountExportSyncFd", history)
        self.assertIn("framegenBatchCompleteValid = true", history)
        self.assertIn("historyRequiresHostCompletionWait", history)
        self.assertNotIn("submitAndWaitForAhbHandoff", history)

    def test_fixed_mode_restores_source_governor_without_source_pacing(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        scheduler = (ROOT / "include/adaptive_scheduler.hpp").read_text(
            encoding="utf-8"
        )
        scheduler_source = (ROOT / "src/adaptive_scheduler.cpp").read_text(
            encoding="utf-8"
        )
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")

        self.assertIn("FixedSourceCadenceGovernor", scheduler)
        self.assertIn("fixedSourceCadenceGovernor_", header)
        self.assertIn("fixedSourceCadenceGovernor_.plan(", source)
        self.assertIn("fixed_generation_limit=", source)
        self.assertIn("FixedSourceCadenceGovernor::plan", scheduler_source)
        self.assertNotIn("sleep_for", scheduler + scheduler_source)

        pacing_start = hooks.index("bool adaptivePresentationPacing")
        pacing_end = hooks.index("bool requiresSwapchainRecreation", pacing_start)
        pacing = hooks[pacing_start:pacing_end]
        self.assertIn("return false;", pacing)
        self.assertNotIn("conf.adaptiveFramegen", pacing)
        self.assertNotIn("conf.adaptiveFlowScale", pacing)

        admission_start = source.index("// Active deadline admission")
        admission_end = source.index(
            "if (this->currentSourceTimeline_.valid)", admission_start
        )
        admission = source[admission_start:admission_end]
        self.assertIn("conf.adaptiveFramegen || sourceProtectionBatchAdmission", admission)

    def test_adaptive_path_uses_variable_count_without_owning_source_pacing(self) -> None:
        scheduler_header = (ROOT / "include/adaptive_scheduler.hpp").read_text(encoding="utf-8")
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")

        self.assertIn("AdaptiveFrameScheduler adaptiveScheduler_", header)
        self.assertIn("AdaptiveSchedulerTelemetry", scheduler_header)
        self.assertIn("lastGeneratedFrameCount", header)
        self.assertNotIn("delayUntilNextSourceOutput", scheduler_header)
        for token in (
            "adaptiveScheduler_.configure",
            "adaptiveScheduler_.plan(sourceInterval)",
            "adaptiveScheduler_.telemetry()",
            "presentContextWithCount",
            "AndroidFrameCycleMode::HistoryOnly",
            "stage=history-only",
        ):
            self.assertIn(token, source)

        self.assertNotIn("std::this_thread::sleep_for(delay)", source)
        self.assertIn("state->context->lastGeneratedFrameCount()", hooks)
        self.assertIn('"adaptive="', hooks)
        self.assertIn('"target_fps="', hooks)

    def test_suspend_boundaries_are_excluded_from_runtime_timing_metrics(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn("kRuntimeTimingDiscontinuityMs = 250.0", source)
        self.assertIn("if (sourceIntervalMs < kRuntimeTimingDiscontinuityMs)", source)
        self.assertIn("bool excludeCurrentCycleFromTimingMetrics = false", source)
        self.assertIn("cycleMs >= kRuntimeTimingDiscontinuityMs", source)
        self.assertIn("excludeCurrentCycleFromTimingMetrics = true", source)
        self.assertIn("runtime-timing-discontinuity", source)
        self.assertIn("action=reset-window", source)
        self.assertIn("metrics.windowWaitIdleMs = 0.0", source)
        self.assertIn("metrics.windowDispatchMs = 0.0", source)
        self.assertIn("if (!excludeCurrentCycleFromTimingMetrics)", source)

    def test_framegen_runtime_reconfigures_instead_of_reusing_incompatible_outputs(self) -> None:
        """Regression: adaptive 4x left generationCount=3 active for fixed 2x's one AHB."""
        for backend in ("v3.1_src", "v3.1p_src"):
            source = (ROOT / "framegen" / backend / "lsfg.cpp").read_text(encoding="utf-8")

            self.assertIn("RuntimeSignature", source)
            self.assertIn("requestedSignature == activeSignature", source)
            self.assertIn("contexts.empty()", source)
            self.assertNotIn(
                "if (instance.has_value() || device.has_value())\n        return;",
                source,
            )
            self.assertIn("output-count mismatch", source)
            self.assertIn("active generation count exceeds runtime capacity", source)

    def test_last_context_release_clears_framegen_runtime(self) -> None:
        """Regression: disabling LSFG must not leave its VkDevice/resources resident."""
        for backend in ("v3.1_src", "v3.1p_src"):
            source = (ROOT / "framegen" / backend / "lsfg.cpp").read_text(encoding="utf-8")

            delete_start = source.index(f"void LSFG_3_1{'P' if 'v3.1p' in backend else ''}::deleteContext")
            finalize_start = source.index(f"void LSFG_3_1{'P' if 'v3.1p' in backend else ''}::finalize", delete_start)
            delete_body = source[delete_start:finalize_start]
            self.assertIn("if (contexts.empty())", delete_body)
            self.assertIn("resetRuntime", delete_body)
            self.assertIn("pendingContextDeletes", source)
            self.assertIn("collectCompletedContextDeletes", source)
            diagnostics_start = source.index(f"{'LSFG_3_1P' if 'v3.1p' in backend else 'LSFG_3_1'}::getBackendDiagnostics")
            diagnostics_end = source.index("int32_t", diagnostics_start)
            self.assertIn("scoped_lock lock(runtimeMutex)", source[diagnostics_start:diagnostics_end])

    def test_adaptive_zero_generation_crosses_handoff_and_advances_history(self) -> None:
        """Fractional zero-generation cadence fills the same temporal history ring."""
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        present_start = source.index("VkResult LsContext::present")
        handoff_start = source.index("submitAndWaitForAhbHandoff", present_start)
        self.assertIn("AndroidFrameCycleMode::HistoryOnly", source)
        history_start = source.index("if (historyOnly)", handoff_start)
        generation_start = source.index(
            "// 2. Tell framegen to generate intermediary frames.", history_start
        )
        history_block = source[history_start:generation_start]

        self.assertGreater(history_start, handoff_start)
        self.assertIn("presentContextWithCountExportSyncFd", history_block)
        self.assertIn("--this->sourceHistoryWarmupRemaining_", history_block)
        self.assertIn("history_warmup_remaining=", history_block)
        self.assertNotIn("source-direct-present", source[present_start:handoff_start])


    def test_generation_resumes_with_legacy_adreno_warmup_and_modern_generic_history(self) -> None:
        """Adreno keeps the Sep-18 single source warmup; generic/Xclipse keeps modern history."""
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("kSourceHistoryWarmupFrames = 4", header)
        self.assertIn(
            "sourceHistoryWarmupRemaining_{kSourceHistoryWarmupFrames}",
            header,
        )
        self.assertIn("requiresSourceHistoryWarmup_{true}", header)

        begin = source.index("// BEGIN ADRENO_364178AF_EXECUTION")
        end = source.index("// END ADRENO_364178AF_EXECUTION", begin)
        adreno = source[begin:end]
        self.assertIn("if (sourceHistoryWarmupActive)", adreno)
        self.assertIn("runtime stage=source-history-warmup", adreno)
        self.assertIn("this->sourceHistoryWarmupRemaining_ = 0;", adreno)
        self.assertIn("this->requiresSourceHistoryWarmup_ = false;", adreno)
        self.assertNotIn("kConservativeSourceReprimeFrames", adreno)
        self.assertNotIn("conservativeFramegenSourceIndex_", adreno)

        # The modern generic/Xclipse path remains present after the island.
        generic = source[end:]
        self.assertIn("if (historyOnly)", generic)
        self.assertIn("sourceHistoryWarmupRemaining_", generic)
        self.assertIn("presentContextWithCountExportSyncFd", generic)

    def test_fixed_multiplier_never_underflows_when_runtime_is_off(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn(
            "conf.multiplier > 1\n            ? static_cast<size_t>(conf.multiplier - 1)",
            source,
        )


    def test_adreno_residency_transform_tracks_pending_context_state(self) -> None:
        script = (ROOT / "scripts/adreno_android_runtime_residency.py").read_text(
            encoding="utf-8"
        )
        self.assertIn("pendingContextDeletes", script)
        self.assertIn("std::mutex runtimeMutex", script)
        self.assertIn("pendingContextDeletes.erase(id)", script)
        self.assertIn("state_marker", script)
        self.assertIn("delete_marker", script)


    def test_context_creation_failure_recreates_original_swapchain(self) -> None:
        """Regression: failed LSFG setup must not leave a modified swapchain contextless."""
        source = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        catch_start = source.index("init stage=ls-context-failed")
        catch_end = source.index("return VK_SUCCESS;", catch_start) + len("return VK_SUCCESS;")
        fallback = source[catch_start:catch_end]

        self.assertIn("ovkDestroySwapchainKHR", fallback)
        self.assertIn("retireSwapchainState(pCreateInfo->oldSwapchain)", fallback)
        self.assertIn("ovkCreateSwapchainKHR(", fallback)
        self.assertIn("fallbackCreateInfo", fallback)
        self.assertIn("swapchain-fallback-pass-through", fallback)

    def test_context_creation_failure_retires_old_wrapper_before_fallback_returns(self) -> None:
        source = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        catch_start = source.index("const VkSwapchainKHR failedSwapchain")
        catch_end = source.index("return VK_SUCCESS;", catch_start)
        fallback = source[catch_start:catch_end]
        self.assertLess(
            fallback.index("fallbackRes"),
            fallback.index("retireSwapchainState(pCreateInfo->oldSwapchain)"),
        )
        self.assertIn("retireSwapchainState(failedSwapchain)", fallback)

    def test_game_config_keeps_target_resident_while_multiplier_one_is_off(self) -> None:
        """GameNative Off remains a resident layer target; multiplier=1 is pass-through."""
        source = (ROOT / "src/config/config.cpp").read_text(encoding="utf-8")
        self.assertIn("Configuration game{\n            // A matching GameNative target", source)
        self.assertIn(".enable = true", source)
        self.assertIn(".targeted = true", source)
        self.assertIn('toml::find_or(gameTable, "multiplier", 2U)', source)
        self.assertNotIn('.enable = toml::find_or(gameTable, "enabled", true)', source)

    def test_adaptive_target_reload_does_not_recreate_the_swapchain(self) -> None:
        """Limiter adjustments must not create multi-second black transition windows."""
        source = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        self.assertIn("requiresSwapchainRecreation", source)
        self.assertIn("const auto previousConf = conf", source)
        self.assertIn("recreateSwapchain = requiresSwapchainRecreation", source)
        self.assertIn("if (recreateSwapchain)", source)

        helper_start = source.index("bool requiresSwapchainRecreation")
        helper_end = source.index("VkResult myvkCreateInstance", helper_start)
        helper = source[helper_start:helper_end]
        self.assertNotIn("adaptiveFramegen", helper)
        self.assertNotIn("fpsLimit", helper)

    def test_gamenative_resident_target_can_toggle_multiplier_without_recreate(self) -> None:
        """GameNative runtime Off/2x/3x/4x changes stay inside one resident swapchain context."""
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        context = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        helper_start = hooks.index("bool requiresSwapchainRecreation")
        helper_end = hooks.index("bool supportsDeviceExtension", helper_start)
        helper = hooks[helper_start:helper_end]
        self.assertIn("const bool residentTarget = previous.targeted && next.targeted", helper)
        self.assertIn("next.multiplier > residentCapacityMultiplier(previous)", helper)
        self.assertNotIn("previous.multiplier != next.multiplier", helper.split("#endif", 1)[0])

        self.assertIn("kAndroidResidentMaxMultiplier = 4", hooks)
        self.assertIn("residentMultiplier", hooks)
        self.assertIn("activeConf.targeted", hooks)
        self.assertIn("kAndroidResidentMaxMultiplier = 4", context)
        self.assertIn("size_t residentCapacityMultiplier", context)
        self.assertIn("const size_t runtimeMultiplier = residentCapacityMultiplier(conf)", context)
        self.assertIn("activeConf.multiplier <= 1 && !activeConf.targeted", hooks)
        self.assertIn("if (conf.targeted && conf.multiplier <= 1)", hooks)
        self.assertIn("state->context->enterSourceOnlyBypass()", hooks)
        self.assertIn("const bool generationActive = activeConf.multiplier > 1", hooks)
        self.assertIn('generationActive ? "generating" : "source_only"', hooks)
        self.assertIn("runtime stage=config-reload-soft-toggle", hooks)
        self.assertIn("recreateSwapchain=0", hooks)

    def test_syncfd_source_export_failure_recreates_temporal_context(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        start = source.index("if (asyncExportFailed)")
        end = source.index("if (historyOnly)", start)
        recovery = source[start:end]

        self.assertIn("Layer::ovkQueuePresentKHR", recovery)
        self.assertIn("VK_ERROR_OUT_OF_DATE_KHR", recovery)
        self.assertIn("pre-copy-syncfd-fail-open-recreate", recovery)
        self.assertIn("kSourceHistoryWarmupFrames", recovery)
        self.assertNotIn(
            'finishSourcePresent(failOpenResult, "pre-copy-syncfd-fail-open")',
            recovery,
        )


if __name__ == "__main__":
    unittest.main()
