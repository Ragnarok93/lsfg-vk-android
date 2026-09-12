#!/usr/bin/env python3
"""Inject temporary zero-generation GPU stage profiling into Android builds.

This profiling transform is intentionally isolated from the checked-in framegen
algorithm. It adds timestamp queries around Mipmaps and Alpha6..Alpha0 only on
zero-generation cycles, without adding submissions, fences, or wait edges.
"""

from __future__ import annotations

import argparse
from pathlib import Path


HEADER_PATHS = (
    Path("framegen/v3.1_include/v3_1/context.hpp"),
    Path("framegen/v3.1p_include/v3_1p/context.hpp"),
)
SOURCE_PATHS = (
    (Path("framegen/v3.1_src/context.cpp"), "quality"),
    (Path("framegen/v3.1p_src/context.cpp"), "performance"),
)


def replace_exact(text: str, old: str, new: str, *, count: int, label: str) -> str:
    if new in text:
        return text
    found = text.count(old)
    if found != count:
        raise RuntimeError(f"{label}: expected {count} match(es), found {found}")
    return text.replace(old, new, count)


def patch_header(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    text = replace_exact(
        text,
        '#include "core/commandbuffer.hpp"\n',
        '#include "core/commandbuffer.hpp"\n#include "core/timestampquerypool.hpp"\n',
        count=1,
        label=f"{path}: timestamp include",
    )
    text = replace_exact(
        text,
        "        std::array<RenderData, 8> data;\n\n        Shaders::Mipmaps mipmaps;",
        "        std::array<RenderData, 8> data;\n\n"
        "        Core::TimestampQueryPool zeroStageQueryPool;\n"
        "        std::array<double, 8> zeroStageProfileTotalsMs{};\n"
        "        uint32_t zeroStageProfileSamples{0};\n\n"
        "        Shaders::Mipmaps mipmaps;",
        count=1,
        label=f"{path}: profiler members",
    )
    path.write_text(text, encoding="utf-8")


def patch_source(path: Path, backend: str) -> None:
    text = path.read_text(encoding="utf-8")
    if f"zero-stage-profile backend={backend}" in text:
        return

    text = replace_exact(
        text,
        "#include <cstdlib>\n",
        "#include <cstdlib>\n#include <iostream>\n",
        count=1,
        label=f"{path}: iostream include",
    )

    text = replace_exact(
        text,
        "        data.cmdBuffers2.resize(vk.generationCount);\n"
        "    }\n\n"
        "    this->mipmaps = Shaders::Mipmaps",
        "        data.cmdBuffers2.resize(vk.generationCount);\n"
        "    }\n"
        "    this->zeroStageQueryPool = Core::TimestampQueryPool(vk.device, 9);\n\n"
        "    this->mipmaps = Shaders::Mipmaps",
        count=2,
        label=f"{path}: query-pool initialization",
    )

    old_dispatch = (
        "    this->mipmaps.Dispatch(data.cmdBuffer1, this->frameIdx);\n"
        "    for (size_t i = 0; i < 7; i++)\n"
        "        this->alpha.at(6 - i).Dispatch(data.cmdBuffer1, this->frameIdx);\n"
        "    if (generationCount > 0)\n"
        "        this->beta.Dispatch(data.cmdBuffer1, this->frameIdx);\n"
    )
    new_dispatch = (
        "    const bool profileZeroStage = generationCount == 0\n"
        "        && this->zeroStageQueryPool.supported();\n"
        "    if (profileZeroStage) {\n"
        "        this->zeroStageQueryPool.reset(data.cmdBuffer1.handle());\n"
        "        this->zeroStageQueryPool.write(data.cmdBuffer1.handle(), 0);\n"
        "    }\n\n"
        "    this->mipmaps.Dispatch(data.cmdBuffer1, this->frameIdx);\n"
        "    if (profileZeroStage)\n"
        "        this->zeroStageQueryPool.write(data.cmdBuffer1.handle(), 1);\n"
        "    for (size_t i = 0; i < 7; i++) {\n"
        "        this->alpha.at(6 - i).Dispatch(data.cmdBuffer1, this->frameIdx);\n"
        "        if (profileZeroStage)\n"
        "            this->zeroStageQueryPool.write(\n"
        "                data.cmdBuffer1.handle(), static_cast<uint32_t>(i + 2));\n"
        "    }\n"
        "    if (generationCount > 0)\n"
        "        this->beta.Dispatch(data.cmdBuffer1, this->frameIdx);\n"
    )
    text = replace_exact(
        text, old_dispatch, new_dispatch, count=1,
        label=f"{path}: stage timestamp recording",
    )

    old_zero_tail = (
        "        if (!data.preprocessingFence.wait(vk.device, framegenWaitTimeoutNs()))\n"
        "            throw LSFG::vulkan_error(VK_TIMEOUT,\n"
        "                \"Temporal preprocessing fence wait timed out\");\n"
        "        this->frameIdx++;\n"
        "        return;\n"
    )
    new_zero_tail = (
        "        if (!data.preprocessingFence.wait(vk.device, framegenWaitTimeoutNs()))\n"
        "            throw LSFG::vulkan_error(VK_TIMEOUT,\n"
        "                \"Temporal preprocessing fence wait timed out\");\n"
        "        if (profileZeroStage) {\n"
        "            const auto durations = this->zeroStageQueryPool.durationsMs(vk.device);\n"
        "            if (durations.size() == this->zeroStageProfileTotalsMs.size()) {\n"
        "                for (size_t i = 0; i < durations.size(); ++i)\n"
        "                    this->zeroStageProfileTotalsMs.at(i) += durations.at(i);\n"
        "                ++this->zeroStageProfileSamples;\n"
        "                if (this->zeroStageProfileSamples >= 60) {\n"
        "                    const double samples =\n"
        "                        static_cast<double>(this->zeroStageProfileSamples);\n"
        "                    double gpuTotalMs = 0.0;\n"
        "                    for (const double totalMs : this->zeroStageProfileTotalsMs)\n"
        "                        gpuTotalMs += totalMs;\n"
        f"                    std::cerr << \"lsfg-vk: zero-stage-profile backend={backend}\"\n"
        "                        << \" samples=\" << this->zeroStageProfileSamples\n"
        "                        << \" mipmaps_avg_ms=\"\n"
        "                        << (this->zeroStageProfileTotalsMs.at(0) / samples)\n"
        "                        << \" alpha6_avg_ms=\"\n"
        "                        << (this->zeroStageProfileTotalsMs.at(1) / samples)\n"
        "                        << \" alpha5_avg_ms=\"\n"
        "                        << (this->zeroStageProfileTotalsMs.at(2) / samples)\n"
        "                        << \" alpha4_avg_ms=\"\n"
        "                        << (this->zeroStageProfileTotalsMs.at(3) / samples)\n"
        "                        << \" alpha3_avg_ms=\"\n"
        "                        << (this->zeroStageProfileTotalsMs.at(4) / samples)\n"
        "                        << \" alpha2_avg_ms=\"\n"
        "                        << (this->zeroStageProfileTotalsMs.at(5) / samples)\n"
        "                        << \" alpha1_avg_ms=\"\n"
        "                        << (this->zeroStageProfileTotalsMs.at(6) / samples)\n"
        "                        << \" alpha0_avg_ms=\"\n"
        "                        << (this->zeroStageProfileTotalsMs.at(7) / samples)\n"
        "                        << \" gpu_total_avg_ms=\" << (gpuTotalMs / samples)\n"
        "                        << '\\n';\n"
        "                    this->zeroStageProfileTotalsMs.fill(0.0);\n"
        "                    this->zeroStageProfileSamples = 0;\n"
        "                }\n"
        "            }\n"
        "        }\n"
        "        this->frameIdx++;\n"
        "        return;\n"
    )
    text = replace_exact(
        text, old_zero_tail, new_zero_tail, count=1,
        label=f"{path}: result collection",
    )

    path.write_text(text, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="repository root to patch",
    )
    args = parser.parse_args()
    root = args.root.resolve()

    for rel in HEADER_PATHS:
        patch_header(root / rel)
    for rel, backend in SOURCE_PATHS:
        patch_source(root / rel, backend)


if __name__ == "__main__":
    main()
