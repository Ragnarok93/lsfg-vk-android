#!/usr/bin/env python3
import unittest
from pathlib import Path

from android_lifecycle_regression_test import AndroidLifecycleRegressionTest  # noqa: F401

ROOT = Path(__file__).resolve().parents[1]


class DelayedWsiConsumer:
    """Pass slots recycle while WSI semaphore owners follow swapchain images."""

    def __init__(self) -> None:
        self.handles_alive = set()
        self.producer_complete = set()
        self.image_consumers = {}
        self.slot_generation = {}

    def submit(self, generation: int, image: int) -> bool:
        slot = generation % 8
        old_generation = self.slot_generation.get(slot)
        if old_generation is not None and old_generation not in self.producer_complete:
            return False
        self.handles_alive.add(generation)
        self.image_consumers[image] = generation
        self.slot_generation[slot] = generation
        return True

    def complete_producer(self, generation: int) -> None:
        self.producer_complete.add(generation)

    def recycle_slot(self, generation: int) -> None:
        # Producer-owned command resources retire here. The modeled semaphore
        # handle remains live through image_consumers.
        pass

    def reacquire(self, image: int) -> None:
        generation = self.image_consumers.pop(image, None)
        if generation is not None:
            self.handles_alive.discard(generation)


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

        self.assertIn("PFN_vkWaitForFences waitHandoffFences", header)
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
        self.assertIn("postCopyCompletionFences", header)
        self.assertIn("postCopyCompletionFenceSubmitted", header)
        self.assertIn("tryRecyclePass", source)
        self.assertIn("VK_TRUE, 0", source)
        self.assertIn("postCopyCompletionFenceSubmitted.at(i) = true", source)
        self.assertIn("completionFenceSubmitted = true", source)
        self.assertNotIn("submitPassCompletionFence", source)
        self.assertNotIn("commandBufferCount = 0", source)

    def test_producer_fence_is_not_total_consumer_retirement(self) -> None:
        """WSI waits stay owned by their image until that image is reacquired."""
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("WsiConsumerResources", header)
        self.assertIn("wsiConsumersByImage_", header)
        self.assertIn("releaseWsiConsumersForImage", source)
        self.assertIn("retainWsiConsumersForImage", source)
        self.assertIn("wsi-image-reacquired", source)
        recycle_start = source.index("bool LsContext::tryRecyclePass")
        recycle_end = source.index("VkResult LsContext::present", recycle_start)
        recycle = source[recycle_start:recycle_end]
        self.assertIn("pass.completionFenceSubmitted = false", recycle)
        self.assertIn("releasePassResources(passIndex, pass)", recycle)
        self.assertNotIn("anchorDeferredRetirements", source)

    def test_real_source_submission_does_not_claim_wsi_retirement(self) -> None:
        """A later queue fence is not used as proof that a present wait retired."""
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        submit_start = source.index("submitAhbHandoff(pass.preCopyBuf")
        submit_end = source.index("metrics.windowHandoffMs", submit_start)
        source_submit = source[submit_start:submit_end]

        self.assertIn("pass.completionFence", source_submit)
        self.assertNotIn("anchorDeferredRetirements", source_submit)
        self.assertNotIn("commandBufferCount = 0", source)
        self.assertNotIn("empty retirement", source.lower())

    def test_every_lsfg_present_wait_is_retained_by_presented_image(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        for reason in (
            '"source-history"',
            '"generated"',
            '"source-final"',
            '"source-syncfd-fallback"',
            '"source-timeout-fallback"',
        ):
            self.assertIn(reason, source)
        self.assertIn("retainWsiConsumersForImage(imageIdx", source)
        self.assertIn("retainWsiConsumersForImage(presentIdx", source)

    def test_source_dependency_tracks_actual_last_submit_not_logical_previous_slot(self) -> None:
        """Source-only fallback must not turn frameIdx-1 into a fake semaphore producer."""
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("LastSourceCopyDependency", header)
        self.assertIn("lastSourceCopyDependency_", header)
        self.assertIn("lastSourceCopyDependency_", source)
        self.assertIn("lastBatchCompleteDependency_", source)
        self.assertNotIn(
            "passInfos.at((this->frameIdx - 1) % 8)",
            source,
            "source semaphore ownership must not be inferred from frameIdx",
        )
        self.assertIn("source-only present", source)

    def test_consuming_pass_owns_queue_wait_semaphores_until_its_fence(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn("queueConsumerSemaphores", header)
        self.assertGreaterEqual(
            source.count("pass.queueConsumerSemaphores.emplace_back("), 3
        )
        self.assertIn("pass.queueConsumerSemaphores.clear()", source)

    def test_delayed_consumer_model_survives_ring_wrap(self) -> None:
        consumer = DelayedWsiConsumer()
        accepted = []
        for _ in range(8 + 1):
            generation = _
            if not consumer.submit(generation, generation):
                continue
            accepted.append(generation)
            consumer.complete_producer(generation)
            consumer.recycle_slot(generation)

        # Producer completion permits slot zero reuse on generation eight, but
        # all nine WSI semaphore handles remain alive without reacquisition.
        self.assertEqual(list(range(9)), accepted)
        self.assertEqual(set(range(9)), consumer.handles_alive)
        for image in range(9):
            consumer.reacquire(image)
        self.assertEqual(set(), consumer.handles_alive)

    def test_slot_fence_reuse_is_independent_from_wsi_consumer_retirement(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn("wsiConsumersByImage_", source)
        self.assertIn("releasePassResources(passIndex, pass)", source)
        self.assertNotIn("retirement-fence-still-referenced", source)

    def test_conservative_retirement_is_explicit_diagnostic_mode(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn("conservativeRetirement_{false}", header)
        self.assertIn("LSFG_VK_RETIREMENT_POLICY", source)
        self.assertIn('"conservative-host"', source)
        self.assertIn('retirement-policy=', source)
        conservative_start = source.index("if (this->conservativeRetirement_")
        conservative_end = source.index(
            "const auto waitAndResetFence", conservative_start
        )
        conservative = source[conservative_start:conservative_end]
        self.assertIn("waitQueueIdle_(this->queue_)", conservative)
        self.assertNotIn("waitQueueIdle_(this->queue_)", source[
            source.index("VkResult LsContext::present"):source.index("#ifdef __ANDROID__", source.index("VkResult LsContext::present"))
        ])

    def test_delayed_consumer_ring_wrap_has_a_deterministic_contract(self) -> None:
        """The regression harness must model delayed consumers beyond eight slots."""
        test_source = Path(__file__).read_text(encoding="utf-8")
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")

        self.assertIn("DelayedWsiConsumer", test_source)
        self.assertIn("for _ in range(8 + 1)", test_source)
        self.assertIn("handles_alive", test_source)
        self.assertIn("wsiConsumersByImage_", header)
        self.assertIn("WsiConsumerResources", header)

    def test_context_teardown_waits_the_game_queue_before_freeing_resources(self) -> None:
        """Regression: swapchain retirement must not free pass handles under GPU use."""
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        destructor_start = source.index("LsContext::~LsContext()")
        present_start = source.index("VkResult LsContext::present", destructor_start)
        destructor = source[destructor_start:present_start]

        self.assertIn("queueWaitIdle", destructor)
        self.assertIn("waitQueueIdle_", destructor)
        self.assertIn("presentQueues_", destructor)
        self.assertIn('role=" << role', destructor)
        self.assertLess(
            destructor.index("waitQueueIdle_"),
            destructor.index("lsfgCtxId.reset()"),
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
        android_start = wrapper.index("#ifdef __ANDROID__", wrapper.index("VkResult LsContext::present"))
        desktop_start = wrapper.index("#else", android_start)
        android_present = wrapper[android_start:desktop_start]
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

    def test_generated_wsi_acquire_is_opportunistic_and_source_safe(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        android_start = source.index(
            "#ifdef __ANDROID__", source.index("VkResult LsContext::present")
        )
        desktop_start = source.index("#else", android_start)
        android_present = source[android_start:desktop_start]
        generated_start = android_present.index(
            "// 4. Generated presentation is opportunistic."
        )
        source_start = android_present.index(
            "// 5. Present the real game frame", generated_start
        )
        generated = android_present[generated_start:source_start]

        self.assertIn("this->swapchain, 0,", generated)
        self.assertIn("syntheticAdmissionNowNs >= syntheticDesiredTimeNs", generated)
        self.assertIn("stage=generated-deadline-drop", generated)
        self.assertIn("res == VK_NOT_READY || res == VK_TIMEOUT", generated)
        self.assertIn(
            "droppedGeneratedFrames = generatedFrameCount - i", generated
        )
        self.assertIn("metrics.windowGeneratedLateDrops", generated)
        self.assertIn("metrics.totalGeneratedLateDrops", generated)
        self.assertNotIn("runtimeWaitTimeoutNs()", generated)

        source_tail = android_present[source_start:]
        self.assertIn(
            "lastPrevPostCopySemaphore = queuedGeneratedFrameCount > 0",
            source_tail,
        )
        self.assertIn(
            ".pNext = adaptivePresentPNext(\n"
            "            pNext,",
            source_tail,
        )
        self.assertIn(
            ".pNext = adaptivePresentPNext(\n"
            "                nullptr,",
            generated,
        )
        self.assertNotIn("generatedDownstreamPNext", generated)
        self.assertIn(
            "this->lastGeneratedFrameCount_ = queuedGeneratedFrameCount",
            android_present,
        )

    def test_first_source_initializes_both_ahb_inputs(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        present = source[source.index("VkResult LsContext::present"):]
        first_copy = present.index("copySwapchainToExternalAhb")
        duplicate = present.index("if (this->frameIdx == 0)", first_copy)
        second_copy = present.index("copySwapchainToExternalAhb", duplicate)
        self.assertLess(first_copy, duplicate)
        self.assertLess(duplicate, second_copy)
        self.assertIn("this->frame_1.handle()", present[second_copy:second_copy + 400])

    def test_fixed_and_adaptive_discontinuities_rebuild_history(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn("sourceTimelineDiscontinuity", source)
        self.assertIn("hadValidSourceTimeline", source)
        self.assertIn(
            "hadValidSourceTimeline\n"
            "                && !this->currentSourceTimeline_.valid",
            source,
        )
        self.assertGreaterEqual(
            source.count(
                "sourceHistoryWarmupRemaining_ = kSourceHistoryWarmupFrames"
            ),
            3,
        )
        self.assertGreaterEqual(
            source.count("deadlineAdmissionPredictor_.reset()"),
            2,
        )

    def test_zero_generation_history_uses_async_dependency_chain(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        android_start = source.index(
            "#ifdef __ANDROID__", source.index("VkResult LsContext::present")
        )
        desktop_start = source.index("#else", android_start)
        android_present = source[android_start:desktop_start]

        self.assertIn(
            "bool useAsyncHandoff = this->asyncAhbHandoffEnabled_;",
            android_present,
        )
        history_start = android_present.index("if (historyOnly)")
        generation_start = android_present.index(
            "// 2. Tell framegen to generate intermediary frames.", history_start
        )
        history = android_present[history_start:generation_start]
        self.assertIn("presentContextWithCountExportSyncFd", history)
        self.assertIn("framegenBatchCompleteValid = true", history)
        self.assertIn("historyRequiresHostCompletionWait", history)
        self.assertNotIn("submitAndWaitForAhbHandoff", history)

    def test_fixed_mode_preserves_requested_multiplier_and_adaptive_flow_does_not_own_pacing(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        scheduler = (ROOT / "include/adaptive_scheduler.hpp").read_text(
            encoding="utf-8"
        )
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")

        self.assertNotIn("FixedSourceCadenceGovernor", scheduler)
        self.assertNotIn("fixedSourceCadenceGovernor_", header)
        self.assertIn(
            ": requestedFixedGeneratedFrameCount;",
            source,
        )
        self.assertIn("fixed_requested_generated=", source)
        self.assertNotIn("fixed_generation_limit=", source)

        pacing_start = hooks.index("bool adaptivePresentationPacing")
        pacing_end = hooks.index("bool requiresSwapchainRecreation", pacing_start)
        pacing = hooks[pacing_start:pacing_end]
        self.assertIn("return false;", pacing)
        self.assertNotIn("conf.adaptiveFramegen", pacing)
        self.assertNotIn("conf.adaptiveFlowScale", pacing)

        context = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn("adaptiveDisplayTimingEnabled_ = false", context)
        admission_start = context.index("Active deadline admission")
        admission_end = context.index(
            "if (this->currentSourceTimeline_.valid)", admission_start
        )
        admission_guard = context[admission_start:admission_end]
        self.assertIn("if (conf.adaptiveFramegen", admission_guard)


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


    def test_generation_resumes_only_after_full_source_history_flush(self) -> None:
        """Startup and bypass flush the contaminated first sample plus all three temporal slots."""
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("kSourceHistoryWarmupFrames = 4", header)
        beta = (ROOT / "framegen/v3.1_src/shaders/beta.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn("for (size_t i = 0; i < 3; i++)", beta)
        self.assertIn("firstDescriptorSet.at(frameCount % 3)", beta)
        self.assertIn(
            "sourceHistoryWarmupRemaining_{kSourceHistoryWarmupFrames}",
            header,
        )
        self.assertIn("requiresSourceHistoryWarmup_{true}", header)
        bypass = source[source.index("void LsContext::enterSourceOnlyBypass"):]
        self.assertIn(
            "sourceHistoryWarmupRemaining_ = kSourceHistoryWarmupFrames",
            bypass,
        )
        self.assertIn("requiresSourceHistoryWarmup_ = true", bypass)
        self.assertIn("BYPASS", bypass.upper())
        self.assertIn("Preserve the last actual producer tokens", bypass)

        self.assertIn("const bool sourceHistoryWarmupActive", source)
        self.assertIn("sourceHistoryWarmupActive\n        ||", source)
        self.assertIn("--this->sourceHistoryWarmupRemaining_", source)
        self.assertNotIn("AndroidFrameCycleMode::SourceWarmup", source)
        self.assertNotIn("stage=source-history-warmup", source)
        self.assertIn("if (this->lastSourceCopyDependency_.valid)", source)


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
