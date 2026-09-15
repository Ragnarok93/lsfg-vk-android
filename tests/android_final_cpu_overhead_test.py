#!/usr/bin/env python3
from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REUSE = ROOT / "scripts/apply-android-command-buffer-reuse.py"

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

# Command-buffer reuse is deliberately a final Android source transform so it
# cannot invalidate earlier profiling/async transform anchors.
assert REUSE.is_file(), REUSE
with tempfile.TemporaryDirectory() as td:
    temp = Path(td)
    for relative in ("framegen/v3.1_src/context.cpp", "framegen/v3.1p_src/context.cpp"):
        src = ROOT / relative
        dst = temp / relative
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dst)
    subprocess.run([sys.executable, str(REUSE), "--root", str(temp)], check=True)
    subprocess.run([sys.executable, str(REUSE), "--root", str(temp)], check=True)
    context = (temp / "framegen/v3.1p_src/context.cpp").read_text(encoding="utf-8")

assert "android-command-buffer-reuse" in context
assert "data.cmdBuffer1 = Core::CommandBuffer(vk.device, vk.commandPool);" in context
assert "cmdBuffer = Core::CommandBuffer(vk.device, vk.commandPool);" in context
assert "data.cmdBuffer1.reset();" in context
assert "buf2.reset();" in context

# Inspect only present(). A second Android Context constructor appears later in
# this source file and intentionally retains the one-time slot allocations.
present_start = context.index("void Context::present(")
present_end = context.index("bool Context::waitForLastPresent", present_start)
present = context[present_start:present_end]
assert "data.cmdBuffer1 = Core::CommandBuffer(vk.device, vk.commandPool);" not in present
assert "buf2 = Core::CommandBuffer(vk.device, vk.commandPool);" not in present

build = (ROOT / "scripts/build/android.sh").read_text(encoding="utf-8")
assert 'apply-android-command-buffer-reuse.py' in build
assert build.index('apply-candidate-b4-beta4-predicate.py') < build.index('apply-android-command-buffer-reuse.py')

print("Final Android CPU overhead optimization contract satisfied")
