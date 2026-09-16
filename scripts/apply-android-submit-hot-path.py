#!/usr/bin/env python3
"""Remove small hot-path heap allocations from Android framegen submission.

The transform is Android build composition only.  It preserves the existing
CommandBuffer::submit API and synchronization semantics while:

* keeping caller-owned semaphore vectors in each RenderData slot so their
  capacity survives across presents; and
* using bounded inline Vulkan submit arrays for the common <=8 semaphore case,
  with the original vector behavior retained as an overflow fallback.

The queue submit itself, wait stages, timeline payloads, fences, and semaphore
ordering are unchanged.
"""
from __future__ import annotations

import argparse
from pathlib import Path

from adreno_evidence_common import replace_exact

CONTEXT_HEADERS = (
    Path("framegen/v3.1_include/v3_1/context.hpp"),
    Path("framegen/v3.1p_include/v3_1p/context.hpp"),
)
CONTEXT_SOURCES = (
    Path("framegen/v3.1_src/context.cpp"),
    Path("framegen/v3.1p_src/context.cpp"),
)
COMMAND_BUFFER_SOURCE = Path("framegen/src/core/commandbuffer.cpp")


def patch_context_header(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "submitWaitSemaphores" in text:
        return

    text = replace_exact(
        text,
        "            std::vector<Core::Semaphore> outSemaphores; // signaled when each pass is done\n",
        "            std::vector<Core::Semaphore> outSemaphores; // signaled when each pass is done\n"
        "            // Android submission scratch: capacity is retained per render slot.\n"
        "            std::vector<Core::Semaphore> submitWaitSemaphores;\n"
        "            std::vector<Core::Semaphore> submitSignalSemaphores;\n"
        "            std::vector<Core::Semaphore> activeInternalSemaphores;\n",
        count=1,
        label=f"{path}: submit scratch slot state",
    )
    path.write_text(text, encoding="utf-8")


def patch_context_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "android-submit-hot-path" in text:
        return

    init_anchor = "        data.internalSemaphores.resize(vk.generationCount);\n"
    init_replacement = (
        init_anchor
        + "        // android-submit-hot-path: retain small submit-vector capacity per slot.\n"
        "        data.submitWaitSemaphores.reserve(1);\n"
        "        data.submitSignalSemaphores.reserve(1);\n"
        "        data.activeInternalSemaphores.reserve(vk.generationCount);\n"
    )
    text = replace_exact(
        text,
        init_anchor,
        init_replacement,
        count=2,
        label=f"{path}: submit scratch capacity initialization",
    )

    waits_old = (
        "    std::vector<Core::Semaphore> waits = { data.inSemaphore };\n"
        "    if (inSem < 0) waits.clear();\n"
    )
    waits_new = (
        "    auto& waits = data.submitWaitSemaphores;\n"
        "    waits.clear();\n"
        "    if (inSem >= 0)\n"
        "        waits.emplace_back(data.inSemaphore);\n"
    )
    text = replace_exact(
        text,
        waits_old,
        waits_new,
        count=1,
        label=f"{path}: persistent input wait scratch",
    )

    active_old = (
        "    const std::vector<Core::Semaphore> activeInternalSemaphores(\n"
        "        data.internalSemaphores.begin(),\n"
        "        data.internalSemaphores.begin() + static_cast<std::ptrdiff_t>(generationCount));\n"
    )
    active_new = (
        "    auto& activeInternalSemaphores = data.activeInternalSemaphores;\n"
        "    activeInternalSemaphores.assign(\n"
        "        data.internalSemaphores.begin(),\n"
        "        data.internalSemaphores.begin() + static_cast<std::ptrdiff_t>(generationCount));\n"
    )
    text = replace_exact(
        text,
        active_old,
        active_new,
        count=1,
        label=f"{path}: persistent active signal scratch",
    )

    signals_old = (
        "        std::vector<Core::Semaphore> signals;\n"
        "        if (hasOutSemaphore)\n"
        "            signals.emplace_back(outSemaphore);\n"
        "        buf2.submit(vk.device.getComputeQueue(), completionFence,\n"
        "            { internalSemaphore }, std::nullopt,\n"
        "            signals, std::nullopt);\n"
    )
    signals_new = (
        "        auto& signals = data.submitSignalSemaphores;\n"
        "        signals.clear();\n"
        "        if (hasOutSemaphore)\n"
        "            signals.emplace_back(outSemaphore);\n"
        "        waits.clear();\n"
        "        waits.emplace_back(internalSemaphore);\n"
        "        buf2.submit(vk.device.getComputeQueue(), completionFence,\n"
        "            waits, std::nullopt, signals, std::nullopt);\n"
    )
    text = replace_exact(
        text,
        signals_old,
        signals_new,
        count=1,
        label=f"{path}: persistent per-pass submit scratch",
    )
    path.write_text(text, encoding="utf-8")


def patch_command_buffer_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "kInlineSubmitSemaphoreCapacity" in text:
        return

    text = replace_exact(
        text,
        "#include <memory>\n",
        "#include <memory>\n#include <array>\n",
        count=1,
        label=f"{path}: inline-submit array include",
    )

    storage_old = (
        "    const std::vector<VkPipelineStageFlags> waitStages(waitSemaphores.size(),\n"
        "        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);\n"
    )
    storage_new = (
        "    // android-submit-hot-path: generated-frame submits normally carry only\n"
        "    // a handful of semaphores. Keep those conversions off the heap while\n"
        "    // retaining vector fallback for any larger/foreign caller.\n"
        "    constexpr size_t kInlineSubmitSemaphoreCapacity = 8;\n"
        "    std::array<VkPipelineStageFlags, kInlineSubmitSemaphoreCapacity> inlineWaitStages{};\n"
        "    std::array<VkSemaphore, kInlineSubmitSemaphoreCapacity> inlineWaitHandles{};\n"
        "    std::array<VkSemaphore, kInlineSubmitSemaphoreCapacity> inlineSignalHandles{};\n"
        "    std::vector<VkPipelineStageFlags> overflowWaitStages;\n"
        "    std::vector<VkSemaphore> overflowWaitHandles;\n"
        "    std::vector<VkSemaphore> overflowSignalHandles;\n\n"
        "    const VkPipelineStageFlags* waitStageData = nullptr;\n"
        "    const VkSemaphore* waitHandleData = nullptr;\n"
        "    const VkSemaphore* signalHandleData = nullptr;\n\n"
        "    if (waitSemaphores.size() <= kInlineSubmitSemaphoreCapacity) {\n"
        "        for (size_t i = 0; i < waitSemaphores.size(); ++i) {\n"
        "            inlineWaitStages.at(i) = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;\n"
        "            inlineWaitHandles.at(i) = waitSemaphores.at(i).handle();\n"
        "        }\n"
        "        if (!waitSemaphores.empty()) {\n"
        "            waitStageData = inlineWaitStages.data();\n"
        "            waitHandleData = inlineWaitHandles.data();\n"
        "        }\n"
        "    } else {\n"
        "        overflowWaitStages.assign(\n"
        "            waitSemaphores.size(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);\n"
        "        overflowWaitHandles.reserve(waitSemaphores.size());\n"
        "        for (const auto& semaphore : waitSemaphores)\n"
        "            overflowWaitHandles.push_back(semaphore.handle());\n"
        "        waitStageData = overflowWaitStages.data();\n"
        "        waitHandleData = overflowWaitHandles.data();\n"
        "    }\n\n"
        "    if (signalSemaphores.size() <= kInlineSubmitSemaphoreCapacity) {\n"
        "        for (size_t i = 0; i < signalSemaphores.size(); ++i)\n"
        "            inlineSignalHandles.at(i) = signalSemaphores.at(i).handle();\n"
        "        if (!signalSemaphores.empty())\n"
        "            signalHandleData = inlineSignalHandles.data();\n"
        "    } else {\n"
        "        overflowSignalHandles.reserve(signalSemaphores.size());\n"
        "        for (const auto& semaphore : signalSemaphores)\n"
        "            overflowSignalHandles.push_back(semaphore.handle());\n"
        "        signalHandleData = overflowSignalHandles.data();\n"
        "    }\n"
    )
    text = replace_exact(
        text,
        storage_old,
        storage_new,
        count=1,
        label=f"{path}: inline submit storage",
    )

    conversions_old = (
        "    std::vector<VkSemaphore> waitSemaphoresHandles;\n"
        "    waitSemaphoresHandles.reserve(waitSemaphores.size());\n"
        "    for (const auto& semaphore : waitSemaphores)\n"
        "        waitSemaphoresHandles.push_back(semaphore.handle());\n"
        "    std::vector<VkSemaphore> signalSemaphoresHandles;\n"
        "    signalSemaphoresHandles.reserve(signalSemaphores.size());\n"
        "    for (const auto& semaphore : signalSemaphores)\n"
        "        signalSemaphoresHandles.push_back(semaphore.handle());\n\n"
    )
    text = replace_exact(
        text,
        conversions_old,
        "",
        count=1,
        label=f"{path}: remove unconditional handle vectors",
    )

    text = replace_exact(
        text,
        "        .pWaitSemaphores = waitSemaphoresHandles.data(),\n"
        "        .pWaitDstStageMask = waitStages.data(),\n",
        "        .pWaitSemaphores = waitHandleData,\n"
        "        .pWaitDstStageMask = waitStageData,\n",
        count=1,
        label=f"{path}: inline wait submit pointers",
    )
    text = replace_exact(
        text,
        "        .pSignalSemaphores = signalSemaphoresHandles.data()\n",
        "        .pSignalSemaphores = signalHandleData\n",
        count=1,
        label=f"{path}: inline signal submit pointer",
    )
    path.write_text(text, encoding="utf-8")


def apply(root: Path) -> None:
    for rel in CONTEXT_HEADERS:
        patch_context_header(root / rel)
    for rel in CONTEXT_SOURCES:
        patch_context_source(root / rel)
    patch_command_buffer_source(root / COMMAND_BUFFER_SOURCE)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    apply(args.root.resolve())


if __name__ == "__main__":
    main()
