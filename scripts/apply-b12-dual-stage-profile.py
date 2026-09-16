#!/usr/bin/env python3
"""Add low-overhead GPU timing for Mipmaps and p_beta[4].

B12 is evidence instrumentation, not a shader optimization.  It adds two
slot-local two-query timestamp pools to the normal frame-generation path:

* one brackets the Mipmaps dispatch on every source cycle;
* one brackets only the fifth Beta dispatch when generationCount > 0.

Results are read only when an eight-slot RenderData entry is reused, after the
slot's existing completion synchronization (or after the synchronous zero-gen
preprocessing fence).  This avoids adding a current-frame GPU wait.  The
transform does not change shader bytecode, workgroup geometry, barriers,
adaptive scheduling, or generation count.
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
MIPMAP_HEADERS = (
    Path("framegen/v3.1_include/v3_1/shaders/mipmaps.hpp"),
    Path("framegen/v3.1p_include/v3_1p/shaders/mipmaps.hpp"),
)
MIPMAP_SOURCES = (
    Path("framegen/v3.1_src/shaders/mipmaps.cpp"),
    Path("framegen/v3.1p_src/shaders/mipmaps.cpp"),
)
BETA_HEADERS = (
    Path("framegen/v3.1_include/v3_1/shaders/beta.hpp"),
    Path("framegen/v3.1p_include/v3_1p/shaders/beta.hpp"),
)
BETA_SOURCES = (
    Path("framegen/v3.1_src/shaders/beta.cpp"),
    Path("framegen/v3.1p_src/shaders/beta.cpp"),
)


def patch_context_header(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "b12MipmapsQueryPool" in text:
        return

    text = replace_exact(
        text,
        '#include "core/commandbuffer.hpp"\n',
        '#include "core/commandbuffer.hpp"\n#include "core/timestampquerypool.hpp"\n',
        count=1,
        label=f"{path}: timestamp query include",
    )
    text = replace_exact(
        text,
        "            Core::Fence preprocessingFence; // reused for zero-generation temporal preprocessing\n",
        "            Core::Fence preprocessingFence; // reused for zero-generation temporal preprocessing\n"
        "            Core::TimestampQueryPool b12MipmapsQueryPool;\n"
        "            Core::TimestampQueryPool b12Beta4QueryPool;\n"
        "            bool b12MipmapsPending{false};\n"
        "            bool b12Beta4Pending{false};\n",
        count=1,
        label=f"{path}: B12 slot-local query state",
    )
    text = replace_exact(
        text,
        "        std::array<RenderData, 8> data;\n",
        "        std::array<RenderData, 8> data;\n"
        "        double b12MipmapsTotalMs{0.0};\n"
        "        double b12Beta4TotalMs{0.0};\n"
        "        uint32_t b12MipmapsSamples{0};\n"
        "        uint32_t b12Beta4Samples{0};\n",
        count=1,
        label=f"{path}: B12 rolling accumulators",
    )
    path.write_text(text, encoding="utf-8")


def patch_context_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "b12-stage-profile" in text:
        return

    text = replace_exact(
        text,
        "#include <cstdlib>\n",
        "#include <cstdlib>\n#include <iostream>\n",
        count=1,
        label=f"{path}: B12 log include",
    )
    text = replace_exact(
        text,
        "        data.preprocessingFence = Core::Fence(vk.device);\n",
        "        data.preprocessingFence = Core::Fence(vk.device);\n"
        "        data.b12MipmapsQueryPool = Core::TimestampQueryPool(vk.device, 2);\n"
        "        data.b12Beta4QueryPool = Core::TimestampQueryPool(vk.device, 2);\n",
        count=2,
        label=f"{path}: B12 query-pool initialization",
    )

    collection_anchor = (
        "    data.shouldWait = generationCount > 0;\n"
        "    data.generationCount = generationCount;\n"
    )
    collection = (
        "    if (data.b12MipmapsPending) {\n"
        "        const auto durations = data.b12MipmapsQueryPool.durationsMs(vk.device);\n"
        "        if (durations.size() == 1) {\n"
        "            this->b12MipmapsTotalMs += durations.front();\n"
        "            ++this->b12MipmapsSamples;\n"
        "        }\n"
        "        data.b12MipmapsPending = false;\n"
        "    }\n"
        "    if (data.b12Beta4Pending) {\n"
        "        const auto durations = data.b12Beta4QueryPool.durationsMs(vk.device);\n"
        "        if (durations.size() == 1) {\n"
        "            this->b12Beta4TotalMs += durations.front();\n"
        "            ++this->b12Beta4Samples;\n"
        "        }\n"
        "        data.b12Beta4Pending = false;\n"
        "    }\n"
        "    if (this->b12MipmapsSamples >= 120) {\n"
        "        std::cout << \"lsfg-vk: b12-stage-profile mipmaps_samples=\"\n"
        "            << this->b12MipmapsSamples\n"
        "            << \" mipmaps_avg_ms=\"\n"
        "            << (this->b12MipmapsTotalMs\n"
        "                / static_cast<double>(this->b12MipmapsSamples))\n"
        "            << \" beta4_samples=\" << this->b12Beta4Samples\n"
        "            << \" beta4_avg_ms=\"\n"
        "            << (this->b12Beta4Samples > 0\n"
        "                ? this->b12Beta4TotalMs / static_cast<double>(this->b12Beta4Samples)\n"
        "                : 0.0)\n"
        "            << std::endl;\n"
        "        this->b12MipmapsTotalMs = 0.0;\n"
        "        this->b12Beta4TotalMs = 0.0;\n"
        "        this->b12MipmapsSamples = 0;\n"
        "        this->b12Beta4Samples = 0;\n"
        "    }\n"
        + collection_anchor
    )
    text = replace_exact(
        text,
        collection_anchor,
        collection,
        count=1,
        label=f"{path}: B12 delayed result collection",
    )

    dispatch_anchor = (
        "    this->mipmaps.Dispatch(data.cmdBuffer1, this->frameIdx);\n"
        "    for (size_t i = 0; i < 7; i++)\n"
        "        this->alpha.at(6 - i).Dispatch(data.cmdBuffer1, this->frameIdx);\n"
        "    if (generationCount > 0)\n"
        "        this->beta.Dispatch(data.cmdBuffer1, this->frameIdx);\n"
    )
    dispatch_profiled = (
        "    const bool b12ProfileMipmaps = data.b12MipmapsQueryPool.supported();\n"
        "    const bool b12ProfileBeta4 = generationCount > 0\n"
        "        && data.b12Beta4QueryPool.supported();\n"
        "    this->mipmaps.Dispatch(\n"
        "        data.cmdBuffer1, this->frameIdx,\n"
        "        b12ProfileMipmaps ? &data.b12MipmapsQueryPool : nullptr);\n"
        "    for (size_t i = 0; i < 7; i++)\n"
        "        this->alpha.at(6 - i).Dispatch(data.cmdBuffer1, this->frameIdx);\n"
        "    if (generationCount > 0)\n"
        "        this->beta.Dispatch(\n"
        "            data.cmdBuffer1, this->frameIdx,\n"
        "            b12ProfileBeta4 ? &data.b12Beta4QueryPool : nullptr);\n"
        "    data.b12MipmapsPending = b12ProfileMipmaps;\n"
        "    data.b12Beta4Pending = b12ProfileBeta4;\n"
    )
    text = replace_exact(
        text,
        dispatch_anchor,
        dispatch_profiled,
        count=1,
        label=f"{path}: B12 dispatch hooks",
    )
    path.write_text(text, encoding="utf-8")


def patch_mipmaps_header(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "Core::TimestampQueryPool* b12Profile" in text:
        return
    text = replace_exact(
        text,
        '#include "core/commandbuffer.hpp"\n',
        '#include "core/commandbuffer.hpp"\n#include "core/timestampquerypool.hpp"\n',
        count=1,
        label=f"{path}: timestamp query include",
    )
    text = replace_exact(
        text,
        "        void Dispatch(const Core::CommandBuffer& buf, uint64_t frameCount);\n",
        "        void Dispatch(const Core::CommandBuffer& buf, uint64_t frameCount,\n"
        "            Core::TimestampQueryPool* b12Profile = nullptr);\n",
        count=1,
        label=f"{path}: B12 Mipmaps profile signature",
    )
    path.write_text(text, encoding="utf-8")


def patch_mipmaps_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "b12Profile->reset" in text:
        return
    text = replace_exact(
        text,
        "void Mipmaps::Dispatch(const Core::CommandBuffer& buf, uint64_t frameCount) {\n",
        "void Mipmaps::Dispatch(const Core::CommandBuffer& buf, uint64_t frameCount,\n"
        "        Core::TimestampQueryPool* b12Profile) {\n",
        count=1,
        label=f"{path}: B12 Mipmaps implementation signature",
    )
    dispatch = (
        "    this->pipeline.bind(buf);\n"
        "    this->descriptorSets.at(frameCount % 2).bind(buf, this->pipeline);\n"
        "    buf.dispatch(threadsX, threadsY, 1);\n"
    )
    profiled = (
        "    if (b12Profile != nullptr) {\n"
        "        b12Profile->reset(buf.handle());\n"
        "        b12Profile->write(buf.handle(), 0);\n"
        "    }\n"
        "    this->pipeline.bind(buf);\n"
        "    this->descriptorSets.at(frameCount % 2).bind(buf, this->pipeline);\n"
        "    buf.dispatch(threadsX, threadsY, 1);\n"
        "    if (b12Profile != nullptr)\n"
        "        b12Profile->write(buf.handle(), 1);\n"
    )
    text = replace_exact(
        text,
        dispatch,
        profiled,
        count=1,
        label=f"{path}: exact Mipmaps dispatch timestamps",
    )
    path.write_text(text, encoding="utf-8")


def patch_beta_header(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "Core::TimestampQueryPool* b12Profile" in text:
        return
    text = replace_exact(
        text,
        '#include "core/commandbuffer.hpp"\n',
        '#include "core/commandbuffer.hpp"\n#include "core/timestampquerypool.hpp"\n',
        count=1,
        label=f"{path}: timestamp query include",
    )
    text = replace_exact(
        text,
        "        void Dispatch(const Core::CommandBuffer& buf, uint64_t frameCount);\n",
        "        void Dispatch(const Core::CommandBuffer& buf, uint64_t frameCount,\n"
        "            Core::TimestampQueryPool* b12Profile = nullptr);\n",
        count=1,
        label=f"{path}: B12 Beta4 profile signature",
    )
    path.write_text(text, encoding="utf-8")


def patch_beta_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "b12Profile->reset" in text:
        return
    text = replace_exact(
        text,
        "void Beta::Dispatch(const Core::CommandBuffer& buf, uint64_t frameCount) {\n",
        "void Beta::Dispatch(const Core::CommandBuffer& buf, uint64_t frameCount,\n"
        "        Core::TimestampQueryPool* b12Profile) {\n",
        count=1,
        label=f"{path}: B12 Beta4 implementation signature",
    )
    fifth_pass = (
        "    this->pipelines.at(4).bind(buf);\n"
        "    this->descriptorSets.at(3).bind(buf, this->pipelines.at(4));\n"
        "    buf.dispatch(threadsX, threadsY, 1);\n"
    )
    fifth_profiled = (
        "    if (b12Profile != nullptr) {\n"
        "        b12Profile->reset(buf.handle());\n"
        "        b12Profile->write(buf.handle(), 0);\n"
        "    }\n"
        "    this->pipelines.at(4).bind(buf);\n"
        "    this->descriptorSets.at(3).bind(buf, this->pipelines.at(4));\n"
        "    buf.dispatch(threadsX, threadsY, 1);\n"
        "    if (b12Profile != nullptr)\n"
        "        b12Profile->write(buf.handle(), 1);\n"
    )
    text = replace_exact(
        text,
        fifth_pass,
        fifth_profiled,
        count=1,
        label=f"{path}: exact Beta4 dispatch timestamps",
    )
    path.write_text(text, encoding="utf-8")


def apply(root: Path) -> None:
    for rel in CONTEXT_HEADERS:
        patch_context_header(root / rel)
    for rel in CONTEXT_SOURCES:
        patch_context_source(root / rel)
    for rel in MIPMAP_HEADERS:
        patch_mipmaps_header(root / rel)
    for rel in MIPMAP_SOURCES:
        patch_mipmaps_source(root / rel)
    for rel in BETA_HEADERS:
        patch_beta_header(root / rel)
    for rel in BETA_SOURCES:
        patch_beta_source(root / rel)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    apply(args.root.resolve())


if __name__ == "__main__":
    main()
