#!/usr/bin/env python3
from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REUSE = ROOT / "scripts/apply-android-command-buffer-reuse.py"
SUBMIT_HOT_PATH = ROOT / "scripts/apply-android-submit-hot-path.py"

utils_h = (ROOT / "framegen/include/common/utils.hpp").read_text(encoding="utf-8")
utils_cpp = (ROOT / "framegen/src/common/utils.cpp").read_text(encoding="utf-8")
pool_cpp = (ROOT / "framegen/src/core/commandpool.cpp").read_text(encoding="utf-8")
cmd_h = (ROOT / "framegen/include/core/commandbuffer.hpp").read_text(encoding="utf-8")
cmd_cpp = (ROOT / "framegen/src/core/commandbuffer.cpp").read_text(encoding="utf-8")

# BarrierBuilder is instantiated dozens of times per generated pass. Keep its
# small image-barrier storage inline and bounded instead of allocating a vector.
assert "kInlineBarrierCapacity" in utils_h
assert "std::array<VkImageMemoryBarrier2, kInlineBarrierCapacity> barriers" in utils_h
assert "barrierCount" in utils_h
assert "this->barriers.reserve(16)" not in utils_h
assert ".imageMemoryBarrierCount = static_cast<uint32_t>(this->barrierCount)" in utils_cpp
assert "BarrierBuilder capacity exceeded" in utils_cpp

# Per-buffer reset support is explicit and legal only because the pool opts in.
assert "VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT" in pool_cpp
assert "void reset();" in cmd_h
assert "CommandBuffer::reset()" in cmd_cpp
assert "vkResetCommandBuffer" in cmd_cpp
assert "Command buffer is not in Submitted state" in cmd_cpp

# Android-only final transforms are composed after all profiling/shader source
# transforms.  Apply each twice here to prove idempotence as well as the final
# hot present()/submit() shape.
assert REUSE.is_file(), REUSE
assert SUBMIT_HOT_PATH.is_file(), SUBMIT_HOT_PATH
with tempfile.TemporaryDirectory() as td:
    temp = Path(td)
    for relative in (
        "framegen/v3.1_include/v3_1/context.hpp",
        "framegen/v3.1p_include/v3_1p/context.hpp",
        "framegen/v3.1_src/context.cpp",
        "framegen/v3.1p_src/context.cpp",
        "framegen/src/core/commandbuffer.cpp",
    ):
        src = ROOT / relative
        dst = temp / relative
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dst)

    for _ in range(2):
        subprocess.run([sys.executable, str(REUSE), "--root", str(temp)], check=True)
        subprocess.run([sys.executable, str(SUBMIT_HOT_PATH), "--root", str(temp)], check=True)

    context_h = (temp / "framegen/v3.1p_include/v3_1p/context.hpp").read_text(
        encoding="utf-8"
    )
    context = (temp / "framegen/v3.1p_src/context.cpp").read_text(encoding="utf-8")
    transformed_cmd = (temp / "framegen/src/core/commandbuffer.cpp").read_text(
        encoding="utf-8"
    )

assert "android-command-buffer-reuse" in context
assert "android-submit-hot-path" in context
assert "data.cmdBuffer1 = Core::CommandBuffer(vk.device, vk.commandPool);" in context
assert "cmdBuffer = Core::CommandBuffer(vk.device, vk.commandPool);" in context
assert "data.cmdBuffer1.reset();" in context
assert "buf2.reset();" in context

# Per-slot caller scratch retains enough capacity for the exact hot-path shapes:
# one wait, one optional output signal, and all active internal pass signals.
for field in (
    "submitWaitSemaphores",
    "submitSignalSemaphores",
    "activeInternalSemaphores",
):
    assert field in context_h
assert "data.submitWaitSemaphores.reserve(1);" in context
assert "data.submitSignalSemaphores.reserve(1);" in context
assert "data.activeInternalSemaphores.reserve(vk.generationCount);" in context
assert "activeInternalSemaphores.assign(" in context

# Inspect only present(). A second Android Context constructor appears later in
# this source file and intentionally retains the one-time slot allocations.
present_start = context.index("void Context::present(")
present_end = context.index("bool Context::waitForLastPresent", present_start)
present = context[present_start:present_end]
assert "data.cmdBuffer1 = Core::CommandBuffer(vk.device, vk.commandPool);" not in present
assert "buf2 = Core::CommandBuffer(vk.device, vk.commandPool);" not in present
assert "std::vector<Core::Semaphore> waits =" not in present
assert "const std::vector<Core::Semaphore> activeInternalSemaphores(" not in present
assert "std::vector<Core::Semaphore> signals;" not in present
assert "{ internalSemaphore }" not in present
assert "auto& waits = data.submitWaitSemaphores;" in present
assert "auto& signals = data.submitSignalSemaphores;" in present

# CommandBuffer::submit keeps small semaphore conversion storage inline, but it
# must retain a heap fallback so foreign callers with >8 semaphores behave
# identically.  Timeline payloads and queue-submit ordering remain untouched.
assert "kInlineSubmitSemaphoreCapacity = 8" in transformed_cmd
assert "std::array<VkPipelineStageFlags, kInlineSubmitSemaphoreCapacity>" in transformed_cmd
assert "std::array<VkSemaphore, kInlineSubmitSemaphoreCapacity> inlineWaitHandles" in transformed_cmd
assert "std::array<VkSemaphore, kInlineSubmitSemaphoreCapacity> inlineSignalHandles" in transformed_cmd
assert "overflowWaitStages.assign(" in transformed_cmd
assert "overflowWaitHandles.reserve(waitSemaphores.size())" in transformed_cmd
assert "overflowSignalHandles.reserve(signalSemaphores.size())" in transformed_cmd
assert ".pWaitSemaphores = waitHandleData" in transformed_cmd
assert ".pWaitDstStageMask = waitStageData" in transformed_cmd
assert ".pSignalSemaphores = signalHandleData" in transformed_cmd
assert "std::vector<VkSemaphore> waitSemaphoresHandles;" not in transformed_cmd
assert "std::vector<VkSemaphore> signalSemaphoresHandles;" not in transformed_cmd
assert "VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT" in transformed_cmd
assert "VkTimelineSemaphoreSubmitInfo timelineInfo" in transformed_cmd
assert "vkQueueSubmit(queue, 1, &submitInfo" in transformed_cmd

build = (ROOT / "scripts/build/android.sh").read_text(encoding="utf-8")
assert 'apply-android-command-buffer-reuse.py' in build
assert 'apply-android-submit-hot-path.py' in build
assert build.index('apply-candidate-b4-beta4-predicate.py') < build.index('apply-android-command-buffer-reuse.py')
assert build.index('apply-android-command-buffer-reuse.py') < build.index('apply-android-submit-hot-path.py')

print("Final Android CPU overhead optimization contract satisfied")
