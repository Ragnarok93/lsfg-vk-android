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
            "queuedGeneratedFrameCount == 0 ? pNext : nullptr",
            source_tail,
        )
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
        self.assertIn("swapchain.lastGeneratedFrameCount()", hooks)
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


    def test_generation_resumes_only_after_three_source_history_updates(self) -> None:
        """Startup and source-only bypass rebuild all three temporal slots."""
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("kSourceHistoryWarmupFrames = 3", header)
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
        self.assertIn("previousSourceCopySignalValid_ = false", bypass)

        self.assertIn("const bool sourceHistoryWarmupActive", source)
        self.assertIn("sourceHistoryWarmupActive\n        ||", source)
        self.assertIn("--this->sourceHistoryWarmupRemaining_", source)
        self.assertNotIn("AndroidFrameCycleMode::SourceWarmup", source)
        self.assertNotIn("stage=source-history-warmup", source)
        self.assertIn(
            "if (this->previousSourceCopySignalValid_ && previousPass != nullptr)",
            source,
        )


    def test_context_creation_failure_recreates_original_swapchain(self) -> None:
        """Regression: failed LSFG setup must not leave a modified swapchain contextless."""
        source = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        catch_start = source.index("init stage=ls-context-failed")
        catch_end = source.index("return VK_SUCCESS;", catch_start) + len("return VK_SUCCESS;")
        fallback = source[catch_start:catch_end]

        self.assertIn("ovkDestroySwapchainKHR", fallback)
        self.assertIn("eraseSwapchainState", fallback)
        self.assertIn("ovkCreateSwapchainKHR(", fallback)
        self.assertIn("fallbackCreateInfo", fallback)
        self.assertIn("swapchain-fallback-pass-through", fallback)

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
        self.assertNotIn("previous.multiplier != next.multiplier", helper.split("#endif", 1)[0])

        self.assertIn("kAndroidResidentMaxMultiplier = 4", hooks)
        self.assertIn("residentMultiplier", hooks)
        self.assertIn("activeConf.targeted", hooks)
        self.assertIn("kAndroidResidentMaxMultiplier = 4", context)
        self.assertIn("size_t residentCapacityMultiplier", context)
        self.assertIn("const size_t runtimeMultiplier = residentCapacityMultiplier(conf)", context)
        self.assertIn("activeConf.multiplier <= 1 && !activeConf.targeted", hooks)
        self.assertIn("if (conf.targeted && conf.multiplier <= 1)", hooks)
        self.assertIn("swapchain.enterSourceOnlyBypass()", hooks)
        self.assertIn("const bool generationActive = activeConf.multiplier > 1", hooks)
        self.assertIn('generationActive ? "generating" : "source_only"', hooks)
        self.assertIn("runtime stage=config-reload-soft-toggle", hooks)
        self.assertIn("recreateSwapchain=0", hooks)


if __name__ == "__main__":
    unittest.main()
