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
        helper_start = source.index("submitAndWaitForAhbHandoff(")
        helper_end = source.index("} // namespace", helper_start)
        helper = source[helper_start:helper_end]
        self.assertIn("std::shared_ptr<VkFence> ahbHandoffFence", header)
        self.assertIn("PFN_vkResetFences resetHandoffFences", header)
        self.assertIn("resetFences(device, 1, &fence)", helper)
        self.assertIn("runtimeWaitTimeoutNs()", helper)
        self.assertNotIn("UINT64_MAX", helper)
        self.assertNotIn('"vkCreateFence"', helper)
        self.assertNotIn('"vkDestroyFence"', helper)

    def test_present_hook_moves_config_and_stats_io_off_hot_path(self) -> None:
        source = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        present_start = source.index("VkResult myvkQueuePresentKHR")
        present_end = source.index("void myvkDestroySwapchainKHR", present_start)
        present = source[present_start:present_end]

        self.assertIn("void runtimeIoWorker()", source)
        self.assertIn("std::chrono::milliseconds(250)", source)
        self.assertIn("std::vector<VkSemaphore> presentWaitSemaphores", source)
        self.assertIn("auto& semaphores = runtimeStats.presentWaitSemaphores", source)
        self.assertIn("applyPendingRuntimeConfig()", present)
        self.assertNotIn("std::filesystem::exists", present)
        self.assertNotIn("std::filesystem::last_write_time", present)
        self.assertNotIn("std::ofstream", present)
        self.assertNotIn("nextConfigPoll", source)

    def test_cross_device_framegen_completion_is_bounded_without_device_wait_idle(self) -> None:
        wrapper = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        android_start = wrapper.index("#ifdef __ANDROID__", wrapper.index("VkResult LsContext::present"))
        desktop_start = wrapper.index("#else", android_start)
        android_present = wrapper[android_start:desktop_start]
        self.assertIn("submitAndWaitForAhbHandoff", android_present)
        self.assertIn("waitContext", android_present)
        self.assertIn("primaryTimeoutNs", android_present)
        self.assertIn("framegen-completion-primary-timeout", android_present)
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

    def test_adaptive_path_skips_unneeded_gpu_work_and_keeps_binary_signals_valid(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")

        self.assertIn("AdaptiveFrameScheduler adaptiveScheduler_", header)
        self.assertIn("AdaptiveFrameScheduler::StageCosts lastStageCosts_", header)
        self.assertIn("lastGeneratedFrameCount", header)
        for token in (
            "adaptiveScheduler_.configure",
            "adaptiveScheduler_.plan(sourceInterval, this->lastStageCosts_)",
            "presentContextWithCount",
            "if (generatedFrameCount == 0)",
            "pass.preCopySemaphores.at(0).handle(),",
            "pass.preCopySemaphores.at(1).handle(),",
            "return finishSourcePresent(directResult, \"game-render\")",
        ):
            self.assertIn(token, source)

        self.assertIn("swapchain.lastGeneratedFrameCount()", hooks)
        self.assertIn('"adaptive="', hooks)
        self.assertIn('"target_fps="', hooks)

    def test_runtime_stats_export_real_interval_generation_counts(self) -> None:
        source = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        for token in (
            "uint64_t windowSourceFrames{}",
            "uint64_t windowGeneratedFrames{}",
            "double generatedPerSource{}",
            '"source_frames=" << snapshot.windowSourceFrames',
            '"generated_frames=" << snapshot.windowGeneratedFrames',
            '"generated_per_source=" << snapshot.generatedPerSource',
            ".windowSourceFrames = stats.windowSourceFrames",
            ".windowGeneratedFrames = stats.windowGeneratedFrames",
        ):
            self.assertIn(token, source)

        present_start = source.index("VkResult myvkQueuePresentKHR")
        present_end = source.index("void myvkDestroySwapchainKHR", present_start)
        present = source[present_start:present_end]
        self.assertNotIn("std::ofstream", present)
        self.assertNotIn("stats.txt", present)

    def test_source_interval_excludes_previous_lsfg_cycle(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        present_start = source.index("VkResult LsContext::present")
        plan_call = source.index("adaptiveScheduler_.plan", present_start)
        measurement = source[present_start:plan_call]

        self.assertIn("Clock::time_point lastCycleEnd", header)
        self.assertIn("Clock::time_point lastPresentEntry", header)
        self.assertIn("cycleStart - metrics.lastCycleEnd", measurement)
        self.assertIn("cycleStart - metrics.lastPresentEntry", measurement)
        self.assertIn("present_entry_interval_avg_ms", source)
        self.assertIn("game_cadence_fps", source)
        self.assertIn("metrics.lastCycleEnd = cycleEnd", source)
        self.assertNotIn("lastSourcePresent", source)

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

    def test_zero_generation_uses_direct_source_present(self) -> None:
        """Target-matched adaptive frames do not cross the AHB boundary."""
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        present_start = source.index("VkResult LsContext::present")
        handoff_start = source.index("submitAndWaitForAhbHandoff", present_start)
        before_handoff = source[present_start:handoff_start]

        self.assertIn("if (generatedFrameCount == 0)", before_handoff)
        self.assertIn("source-direct-present", before_handoff)
        self.assertIn("Layer::ovkQueuePresentKHR(queue, &directPresentInfo)", before_handoff)
        self.assertIn("return finishSourcePresent", before_handoff)

    def test_generation_resumes_only_after_source_history_warmup(self) -> None:
        """A direct-present run must not interpolate against stale AHB input."""
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("requiresSourceHistoryWarmup_", header)
        self.assertIn("previousSourceCopySignalValid_", header)
        self.assertIn("requiresSourceHistoryWarmup_ = true", source)
        self.assertIn("const bool warmupSourceHistory", source)
        self.assertIn("if (this->previousSourceCopySignalValid_)", source)
        self.assertIn("stage=source-history-warmup", source)
        self.assertIn("noteActualGenerationCount(0)", source)
        self.assertIn("return finishSourcePresent", source)

    def test_context_creation_has_no_config_filesystem_debounce(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        constructor_start = source.index("LsContext::LsContext")
        present_start = source.index("VkResult LsContext::present", constructor_start)
        constructor = source[constructor_start:present_start]
        self.assertNotIn("std::filesystem::exists", constructor)
        self.assertNotIn("std::filesystem::last_write_time", constructor)
        self.assertNotIn("milliseconds(100)", constructor)
        self.assertIn("hooks.cpp owns hot reload", constructor)

    def test_context_creation_failure_recreates_original_swapchain(self) -> None:
        """Failed LSFG setup must not leave a modified swapchain contextless."""
        source = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        catch_start = source.index("init stage=ls-context-failed")
        catch_end = source.index("return VK_SUCCESS;", catch_start) + len("return VK_SUCCESS;")
        fallback = source[catch_start:catch_end]

        self.assertIn("ovkDestroySwapchainKHR", fallback)
        self.assertIn("eraseSwapchainState", fallback)
        self.assertIn("ovkCreateSwapchainKHR(", fallback)
        self.assertIn("fallbackCreateInfo", fallback)
        self.assertIn("swapchain-fallback-pass-through", fallback)
        self.assertIn("SwapchainWsiProvenance", source)

    def test_game_config_separates_hook_targeting_from_runtime_enabled(self) -> None:
        """The hook stays resident while enabled=false is canonical user-facing Off."""
        source = (ROOT / "src/config/config.cpp").read_text(encoding="utf-8")
        self.assertIn('.enable = toml::find_or(gameTable, "enabled", true)', source)
        self.assertIn(".targeted = true", source)
        self.assertIn('toml::find_or(gameTable, "multiplier", 2U)', source)
        self.assertIn('const char* enabled = std::getenv("LSFG_ENABLED")', source)

    def test_adaptive_target_reload_does_not_recreate_the_swapchain(self) -> None:
        """Pure controller target/limiter changes are staged without WSI churn."""
        source = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        helper_start = source.index("bool requiresSwapchainRecreation")
        helper_end = source.index("bool supportsDeviceExtension", helper_start)
        helper = source[helper_start:helper_end]

        self.assertIn("requiresSwapchainRecreation", source)
        self.assertNotIn("adaptiveFramegen", helper)
        self.assertNotIn("fpsLimit", helper)
        self.assertIn("PendingConfigAction::Applied", source)
        self.assertIn("config-applied-no-wsi-recreate", source)

    def test_runtime_disable_restores_unmodified_wsi_and_fast_paths(self) -> None:
        """Runtime Off recreates FG-modified WSI, then sustained Off is native."""
        source = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        helper_start = source.index("bool requiresSwapchainRecreation")
        helper_end = source.index("bool supportsDeviceExtension", helper_start)
        helper = source[helper_start:helper_end]
        present_start = source.index("VkResult myvkQueuePresentKHR")
        present_end = source.index("void myvkDestroySwapchainKHR", present_start)
        present = source[present_start:present_end]

        self.assertIn("previousNeedsFgWsi", helper)
        self.assertIn("nextNeedsFgWsi", helper)
        self.assertIn("previous.multiplier != next.multiplier", helper)
        self.assertIn("previous.e_present != next.e_present", helper)
        self.assertIn("PendingConfigAction::Recreate", present)
        self.assertIn("runtime stage=wsi-restore-requested", present)
        self.assertIn("if (!conf.enable || conf.multiplier <= 1)", present)
        off_fast_path = present.index("if (!conf.enable || conf.multiplier <= 1)")
        context_lookup = present.index("swapchains.find", off_fast_path)
        self.assertLess(off_fast_path, context_lookup)
        self.assertIn("gameMinImageCount", source)
        self.assertIn("effectiveMinImageCount", source)
        self.assertIn("gamePresentMode", source)
        self.assertIn("effectivePresentMode", source)

    def test_generated_and_source_presents_are_output_cadence_spaced(self) -> None:
        """Phase spacing remains, but its cadence is owned by the correct mode."""
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        frame_pacer = (ROOT / "include/frame_pacer.hpp").read_text(encoding="utf-8")
        policy = (ROOT / "include/runtime_policy.hpp").read_text(encoding="utf-8")

        self.assertIn('#include "output_frame_pacer.hpp"', header)
        self.assertIn('#include "runtime_policy.hpp"', source)
        self.assertIn("OutputFramePacer outputFramePacer_", header)
        self.assertIn("resolveOutputPacingTarget(", source)
        self.assertIn("conf.adaptiveFramegen, conf.fpsLimit", source)
        self.assertIn("conf.sourceFpsLimit, conf.multiplier", source)
        self.assertIn("outputFramePacer_.configure(outputPacingTarget)", source)
        self.assertNotIn("outputFramePacer_.configure(conf.fpsLimit)", source)
        self.assertIn("capacityFps = saturatingOutputRate(sourceFpsLimit, multiplier)", policy)
        self.assertIn("return std::min(adaptiveTargetFps, capacityFps)", policy)
        self.assertIn("periodRemainder_", frame_pacer)
        self.assertIn("phaseRemainder_", frame_pacer)
        self.assertIn("waitForFineOutputDeadline", source)
        generated_present = source.index("Layer::ovkQueuePresentKHR(queue, &presentInfo)")
        generated_pacing = source.rfind("paceOutputPresent", 0, generated_present)
        source_present = source.index("Layer::ovkQueuePresentKHR(queue, &finalPresentInfo)")
        source_pacing = source.rfind("paceOutputPresent", 0, source_present)
        self.assertGreater(generated_pacing, source.index("for (size_t i = 0; i < generatedFrameCount"))
        self.assertGreater(source_pacing, generated_present)


if __name__ == "__main__":
    unittest.main()
