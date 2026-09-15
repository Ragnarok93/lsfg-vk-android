#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

utils_h = (ROOT / "framegen/include/common/utils.hpp").read_text(encoding="utf-8")
utils_cpp = (ROOT / "framegen/src/common/utils.cpp").read_text(encoding="utf-8")
pool_cpp = (ROOT / "framegen/src/core/commandpool.cpp").read_text(encoding="utf-8")
cmd_h = (ROOT / "framegen/include/core/commandbuffer.hpp").read_text(encoding="utf-8")
cmd_cpp = (ROOT / "framegen/src/core/commandbuffer.cpp").read_text(encoding="utf-8")
context = (ROOT / "framegen/v3.1p_src/context.cpp").read_text(encoding="utf-8")

# BarrierBuilder is instantiated dozens of times per generated pass. Keep its
# small image-barrier storage inline and bounded instead of allocating a vector.
assert "kInlineBarrierCapacity" in utils_h
assert "std::array<VkImageMemoryBarrier2, kInlineBarrierCapacity> barriers" in utils_h
assert "barrierCount" in utils_h
assert "this->barriers.reserve(16)" not in utils_h
assert ".imageMemoryBarrierCount = static_cast<uint32_t>(this->barrierCount)" in utils_cpp
assert "BarrierBuilder capacity exceeded" in utils_cpp

# Command buffers are owned per eight-frame slot. Allocate them once and reset
# them only after the existing slot/fence retirement guarantees completion.
assert "VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT" in pool_cpp
assert "void reset();" in cmd_h
assert "CommandBuffer::reset()" in cmd_cpp
assert "vkResetCommandBuffer" in cmd_cpp
assert "data.cmdBuffer1 = Core::CommandBuffer(vk.device, vk.commandPool);" in context
assert "cmdBuffer = Core::CommandBuffer(vk.device, vk.commandPool);" in context
assert "data.cmdBuffer1.reset();" in context
assert "buf2.reset();" in context

# The per-frame allocation forms must be gone from present(); constructor
# initialization remains intentionally present.
present = context[context.index("void Context::present("):]
assert "data.cmdBuffer1 = Core::CommandBuffer(vk.device, vk.commandPool);" not in present
assert "buf2 = Core::CommandBuffer(vk.device, vk.commandPool);" not in present

print("Final Android CPU overhead optimization contract satisfied")
