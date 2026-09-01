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
        self.assertIn("LSFG_3_1P::waitIdle();", android_present)
        self.assertIn("LSFG_3_1::waitIdle();", android_present)

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


if __name__ == "__main__":
    unittest.main()
