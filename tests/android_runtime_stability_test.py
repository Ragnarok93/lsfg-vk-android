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

    def test_cross_device_framegen_completion_waits_only_active_context_with_one_budget(self) -> None:
        wrapper = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        android_start = wrapper.index("#ifdef __ANDROID__", wrapper.index("VkResult LsContext::present"))
        desktop_start = wrapper.index("#else", android_start)
        android_present = wrapper[android_start:desktop_start]
        self.assertIn("submitAndWaitForAhbHandoff", android_present)
        self.assertIn("LSFG_3_1P::waitContext(*this->lsfgCtxId)", android_present)
        self.assertIn("LSFG_3_1::waitContext(*this->lsfgCtxId)", android_present)
        self.assertNotIn("LSFG_3_1P::waitIdle();", android_present)
        self.assertNotIn("LSFG_3_1::waitIdle();", android_present)

        for relative in (
            "framegen/v3.1_src/context.cpp",
            "framegen/v3.1p_src/context.cpp",
        ):
            context_source = (ROOT / relative).read_text(encoding="utf-8")
            completion_start = context_source.index("bool Context::waitForCompletion")
            completion = context_source[completion_start:]
            self.assertIn("const auto deadline = std::chrono::steady_clock::now()", completion)
            self.assertIn("deadline - now", completion)
            self.assertIn("static_cast<uint64_t>(remaining)", completion)
            self.assertNotIn("fence.wait(vk.device, framegenWaitTimeoutNs())", completion)

        for relative in (
            "framegen/v3.1_src/lsfg.cpp",
            "framegen/v3.1p_src/lsfg.cpp",
        ):
            lifecycle_source = (ROOT / relative).read_text(encoding="utf-8")
            self.assertIn("bool LSFG_3_1", lifecycle_source)
            self.assertIn("::waitContext(int32_t id)", lifecycle_source)
            self.assertNotIn("vkDeviceWaitIdle", lifecycle_source)

    def test_last_context_release_drops_framegen_runtime_after_bounded_teardown(self) -> None:
        for relative in (
            "framegen/v3.1_src/lsfg.cpp",
            "framegen/v3.1p_src/lsfg.cpp",
        ):
            source = (ROOT / relative).read_text(encoding="utf-8")
            delete_start = source.index("::deleteContext(int32_t id)")
            finalize_start = source.index("::finalize()", delete_start)
            delete_body = source[delete_start:finalize_start]
            self.assertIn("waitForCompletion(*device)", delete_body)
            self.assertIn("contexts.erase(it);", delete_body)
            self.assertIn("if (contexts.empty())", delete_body)
            self.assertIn("device.reset();", delete_body)
            self.assertIn("instance.reset();", delete_body)
            self.assertLess(delete_body.index("contexts.erase(it);"), delete_body.index("device.reset();"))
            self.assertLess(delete_body.index("device.reset();"), delete_body.index("instance.reset();"))

    def test_context_creation_rejects_stale_runtime_output_capacity(self) -> None:
        for relative in (
            "framegen/v3.1_src/lsfg.cpp",
            "framegen/v3.1p_src/lsfg.cpp",
        ):
            source = (ROOT / relative).read_text(encoding="utf-8")
            self.assertIn("void validateOutputCount(size_t outputCount)", source)
            self.assertIn("outputCount != device->generationCount", source)
            self.assertIn("validateOutputCount(outN.size());", source)
            self.assertGreaterEqual(source.count("validateOutputCount(outN.size());"), 2)
            self.assertIn("LSFG output-count mismatch", source)


if __name__ == "__main__":
    unittest.main()
