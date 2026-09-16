#!/usr/bin/env python3
"""Add profiling-only direct timing for the p_beta[4] dispatch.

This transform runs after apply-adreno-evidence-profile.py.  It deliberately
keeps the existing generated-stage aggregate profiler intact and adds one
slot-local two-query timestamp pool around only the fifth Beta dispatch.
That makes B4-vs-B4+B11 comparisons sensitive to p_beta[4] without changing
clean runtime builds or any shader semantics.
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
    if "generatedBeta4QueryPool" in text:
        return
    if "generatedPreQueryPool" not in text:
        raise RuntimeError(f"{path}: generated-stage profiling must be applied first")

    text = replace_exact(
        text,
        "            Core::TimestampQueryPool generatedPreQueryPool;\n",
        "            Core::TimestampQueryPool generatedPreQueryPool;\n"
        "            Core::TimestampQueryPool generatedBeta4QueryPool; // B11 evidence only\n",
        count=1,
        label=f"{path}: Beta4 query-pool slot state",
    )
    text = replace_exact(
        text,
        "        uint32_t generatedProfilePassSamples{0};\n",
        "        uint32_t generatedProfilePassSamples{0};\n"
        "        double generatedBeta4ProfileTotalMs{0.0};\n"
        "        uint32_t generatedBeta4ProfileSamples{0};\n",
        count=1,
        label=f"{path}: Beta4 profile accumulators",
    )
    path.write_text(text, encoding="utf-8")


def patch_context_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "beta4_profile_samples=" in text:
        return
    if "generated-stage-profile backend=" not in text:
        raise RuntimeError(f"{path}: generated-stage profiling must be applied first")

    text = replace_exact(
        text,
        "        data.generatedPreQueryPool = Core::TimestampQueryPool(vk.device, 5);\n",
        "        data.generatedPreQueryPool = Core::TimestampQueryPool(vk.device, 5);\n"
        "        data.generatedBeta4QueryPool = Core::TimestampQueryPool(vk.device, 2);\n",
        count=2,
        label=f"{path}: Beta4 query-pool initialization",
    )
    text = replace_exact(
        text,
        "    bool profileGenerated = generationCount > 0 && data.generatedPreQueryPool.supported();\n",
        "    bool profileGenerated = generationCount > 0\n"
        "        && data.generatedPreQueryPool.supported()\n"
        "        && data.generatedBeta4QueryPool.supported();\n",
        count=1,
        label=f"{path}: Beta4 profiling capability gate",
    )

    collection_anchor = (
        "        const size_t profiledPasses = std::min(\n"
        "            data.generatedProfileGenerationCount, data.generatedPassQueryPools.size());\n"
    )
    collection = (
        "        const auto beta4Durations = data.generatedBeta4QueryPool.durationsMs(vk.device);\n"
        "        if (beta4Durations.size() == 1) {\n"
        "            this->generatedBeta4ProfileTotalMs += beta4Durations.front();\n"
        "            ++this->generatedBeta4ProfileSamples;\n"
        "        }\n"
        + collection_anchor
    )
    text = replace_exact(
        text,
        collection_anchor,
        collection,
        count=1,
        label=f"{path}: Beta4 result collection",
    )

    beta_log = (
        "                << \" beta_avg_ms=\" << (this->generatedPreProfileTotalsMs.at(3) / sourceSamples)\n"
    )
    beta_log_new = (
        beta_log
        + "                << \" beta4_profile_samples=\" << this->generatedBeta4ProfileSamples\n"
        "                << \" beta4_avg_ms=\" << (this->generatedBeta4ProfileSamples > 0\n"
        "                    ? this->generatedBeta4ProfileTotalMs\n"
        "                        / static_cast<double>(this->generatedBeta4ProfileSamples)\n"
        "                    : 0.0)\n"
    )
    text = replace_exact(
        text,
        beta_log,
        beta_log_new,
        count=1,
        label=f"{path}: Beta4 generated-stage log fields",
    )

    reset_anchor = "            this->generatedPassProfileTotalsMs.fill(0.0);\n"
    text = replace_exact(
        text,
        reset_anchor,
        reset_anchor
        + "            this->generatedBeta4ProfileTotalMs = 0.0;\n"
        "            this->generatedBeta4ProfileSamples = 0;\n",
        count=1,
        label=f"{path}: Beta4 accumulator reset",
    )

    dispatch_old = (
        "        this->beta.Dispatch(data.cmdBuffer1, this->frameIdx);\n"
        "        if (profileGenerated)\n"
        "            data.generatedPreQueryPool.write(data.cmdBuffer1.handle(), 4);\n"
    )
    dispatch_new = (
        "        this->beta.Dispatch(\n"
        "            data.cmdBuffer1, this->frameIdx,\n"
        "            profileGenerated ? &data.generatedBeta4QueryPool : nullptr);\n"
        "        if (profileGenerated)\n"
        "            data.generatedPreQueryPool.write(data.cmdBuffer1.handle(), 4);\n"
    )
    text = replace_exact(
        text,
        dispatch_old,
        dispatch_new,
        count=1,
        label=f"{path}: Beta4 profiling dispatch hook",
    )
    path.write_text(text, encoding="utf-8")


def patch_beta_header(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "Core::TimestampQueryPool* beta4Profile" in text:
        return

    text = replace_exact(
        text,
        '#include "core/commandbuffer.hpp"\n',
        '#include "core/commandbuffer.hpp"\n#include "core/timestampquerypool.hpp"\n',
        count=1,
        label=f"{path}: timestamp-query include",
    )
    text = replace_exact(
        text,
        "        void Dispatch(const Core::CommandBuffer& buf, uint64_t frameCount);\n",
        "        void Dispatch(const Core::CommandBuffer& buf, uint64_t frameCount,\n"
        "            Core::TimestampQueryPool* beta4Profile = nullptr);\n",
        count=1,
        label=f"{path}: Beta4 profiling signature",
    )
    path.write_text(text, encoding="utf-8")


def patch_beta_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "beta4Profile->reset" in text:
        return

    text = replace_exact(
        text,
        "void Beta::Dispatch(const Core::CommandBuffer& buf, uint64_t frameCount) {\n",
        "void Beta::Dispatch(const Core::CommandBuffer& buf, uint64_t frameCount,\n"
        "        Core::TimestampQueryPool* beta4Profile) {\n",
        count=1,
        label=f"{path}: Beta4 profiling implementation signature",
    )

    fifth_pass = (
        "    Utils::BarrierBuilder(buf)\n"
        "        .addW2R(this->tempImgs2)\n"
        "        .addR2W(this->outImgs)\n"
        "        .build();\n\n"
        "    this->pipelines.at(4).bind(buf);\n"
        "    this->descriptorSets.at(3).bind(buf, this->pipelines.at(4));\n"
        "    buf.dispatch(threadsX, threadsY, 1);\n"
    )
    fifth_pass_profiled = (
        "    Utils::BarrierBuilder(buf)\n"
        "        .addW2R(this->tempImgs2)\n"
        "        .addR2W(this->outImgs)\n"
        "        .build();\n\n"
        "    if (beta4Profile != nullptr) {\n"
        "        beta4Profile->reset(buf.handle());\n"
        "        beta4Profile->write(buf.handle(), 0);\n"
        "    }\n"
        "    this->pipelines.at(4).bind(buf);\n"
        "    this->descriptorSets.at(3).bind(buf, this->pipelines.at(4));\n"
        "    buf.dispatch(threadsX, threadsY, 1);\n"
        "    if (beta4Profile != nullptr)\n"
        "        beta4Profile->write(buf.handle(), 1);\n"
    )
    text = replace_exact(
        text,
        fifth_pass,
        fifth_pass_profiled,
        count=1,
        label=f"{path}: direct Beta4 dispatch timestamps",
    )
    path.write_text(text, encoding="utf-8")


def apply(root: Path) -> None:
    for rel in CONTEXT_HEADERS:
        patch_context_header(root / rel)
    for rel in CONTEXT_SOURCES:
        patch_context_source(root / rel)
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
