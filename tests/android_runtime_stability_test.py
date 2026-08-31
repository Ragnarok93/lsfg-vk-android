#!/usr/bin/env python3
import unittest
from pathlib import Path

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

    def test_android_handoff_reuses_one_context_fence(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        helper_start = source.index("void submitAndWaitForAhbHandoff(")
        helper_end = source.index("} // namespace", helper_start)
        helper = source[helper_start:helper_end]

        self.assertIn("std::shared_ptr<VkFence> ahbHandoffFence", header)
        self.assertIn("PFN_vkResetFences resetHandoffFences", header)
        self.assertIn("resetFences(device, 1, &fence)", helper)
        self.assertNotIn('"vkCreateFence"', helper)
        self.assertNotIn('"vkDestroyFence"', helper)

    def test_present_hook_debounces_fs_and_reuses_wait_storage(self) -> None:
        source = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        self.assertIn("Clock::time_point nextConfigPoll", source)
        self.assertIn("std::chrono::milliseconds(250)", source)
        self.assertIn("std::vector<VkSemaphore> presentWaitSemaphores", source)
        self.assertIn("auto& semaphores = runtimeStats.presentWaitSemaphores", source)

    def test_cross_device_ahb_completion_uses_context_scoped_fences(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        android_start = source.index("#ifdef __ANDROID__", source.index("VkResult LsContext::present"))
        desktop_start = source.index("#else", android_start)
        android = source[android_start:desktop_start]

        self.assertIn("submitAndWaitForAhbHandoff", android)
        self.assertIn("waitContext", android)
        self.assertIn("FRAMEGEN_COMPLETION_TIMEOUT_NS", android)
        self.assertIn("framegen-completion-timeout", android)
        self.assertNotIn("LSFG_3_1P::waitIdle();", android)
        self.assertNotIn("LSFG_3_1::waitIdle();", android)

    def test_adaptive_path_skips_unneeded_gpu_work_and_keeps_binary_signals_valid(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")

        self.assertIn("AdaptiveFrameScheduler adaptiveScheduler_", header)
        self.assertIn("lastGeneratedFrameCount", header)
        for token in (
            "adaptiveScheduler_.configure",
            "adaptiveScheduler_.plan(sourceInterval)",
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
        """Regression: target-matched adaptive frames must not cross the AHB boundary."""
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        present_start = source.index("VkResult LsContext::present")
        handoff_start = source.index("submitAndWaitForAhbHandoff", present_start)
        before_handoff = source[present_start:handoff_start]

        self.assertIn("if (generatedFrameCount == 0)", before_handoff)
        self.assertIn("source-direct-present", before_handoff)
        self.assertIn("Layer::ovkQueuePresentKHR(queue, &directPresentInfo)", before_handoff)
        self.assertIn("return finishSourcePresent", before_handoff)

    def test_generation_resumes_only_after_source_history_warmup(self) -> None:
        """Regression: a direct-present run must not interpolate against stale AHB input."""
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("requiresSourceHistoryWarmup_", header)
        self.assertIn("previousSourceCopySignalValid_", header)
        self.assertIn("requiresSourceHistoryWarmup_ = true", source)
        self.assertIn("const bool warmupSourceHistory", source)
        self.assertIn("if (this->previousSourceCopySignalValid_)", source)
        self.assertIn("stage=source-history-warmup", source)
        self.assertIn("return finishSourcePresent", source)

    def test_context_creation_failure_recreates_original_swapchain(self) -> None:
        """Regression: failed LSFG setup must not leave a modified swapchain contextless."""
        source = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        catch_start = source.index("init stage=ls-context-failed")
        catch_end = source.index("return VK_SUCCESS;", catch_start) + len("return VK_SUCCESS;")
        fallback = source[catch_start:catch_end]

        self.assertIn("ovkDestroySwapchainKHR", fallback)
        self.assertIn("eraseSwapchainState", fallback)
        self.assertIn("ovkCreateSwapchainKHR(", fallback)
        self.assertIn("device, pCreateInfo, pAllocator, pSwapchain", fallback)
        self.assertIn("swapchain-fallback-pass-through", fallback)

    def test_game_config_supports_explicit_disabled_state(self) -> None:
        """Regression: pass-through must not be encoded as an enabled multiplier-one game."""
        source = (ROOT / "src/config/config.cpp").read_text(encoding="utf-8")
        self.assertIn('.enable = toml::find_or(gameTable, "enabled", true)', source)


if __name__ == "__main__":
    unittest.main()
