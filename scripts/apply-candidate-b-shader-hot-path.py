#!/usr/bin/env python3
"""Candidate B shader hot-path evidence for Android profiling builds.

Runs after the existing zero-stage, Mipmaps-structure, and Adreno evidence
transforms. It does not alter shader bytecode, queue topology, synchronization,
or pacing. It only:
  * extends translated SPIR-V structure logging from Mipmaps to Beta passes;
  * splits the existing aggregate Beta GPU timestamp interval into five passes.
"""
from __future__ import annotations

import argparse
from pathlib import Path


BETA_PATHS = (
    (
        Path("framegen/v3.1_include/v3_1/shaders/beta.hpp"),
        Path("framegen/v3.1_src/shaders/beta.cpp"),
    ),
    (
        Path("framegen/v3.1p_include/v3_1p/shaders/beta.hpp"),
        Path("framegen/v3.1p_src/shaders/beta.cpp"),
    ),
)
FRAMEGEN_HEADERS = (
    Path("framegen/v3.1_include/v3_1/context.hpp"),
    Path("framegen/v3.1p_include/v3_1p/context.hpp"),
)
FRAMEGEN_SOURCES = (
    Path("framegen/v3.1_src/context.cpp"),
    Path("framegen/v3.1p_src/context.cpp"),
)
TRANSLATION_SOURCE = Path("src/extract/trans.cpp")


def replace_exact(text: str, old: str, new: str, *, count: int, label: str) -> str:
    if new in text:
        return text
    found = text.count(old)
    if found != count:
        raise RuntimeError(f"{label}: expected {count} match(es), found {found}")
    return text.replace(old, new, count)


def patch_translation_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "shader-hot-path-profile" in text:
        return

    old_filter = (
        '        if (shaderName != "mipmaps" && shaderName != "p_mipmaps")\n'
        '            return;\n'
    )
    new_filter = (
        '        const bool isMipmapsHotPath = shaderName == "mipmaps"\n'
        '            || shaderName == "p_mipmaps";\n'
        '        const bool isBetaHotPath = shaderName.rfind("beta[", 0) == 0\n'
        '            || shaderName.rfind("p_beta[", 0) == 0;\n'
        '        if (!isMipmapsHotPath && !isBetaHotPath)\n'
        '            return;\n'
    )
    text = replace_exact(
        text, old_filter, new_filter, count=1,
        label=f"{path}: Mipmaps/Beta profile target filter",
    )

    old_log = '        std::cerr << "lsfg-vk: zero-stage-shader-profile"\n'
    new_log = (
        '        std::cerr << "lsfg-vk: shader-hot-path-profile"\n'
        '            << " stage=" << (isBetaHotPath ? "beta" : "mipmaps")\n'
    )
    text = replace_exact(
        text, old_log, new_log, count=1,
        label=f"{path}: Candidate B profile marker",
    )
    path.write_text(text, encoding="utf-8")


def patch_beta_header(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "postPassQueryIndex" in text:
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
        "            Core::TimestampQueryPool* profilePool = nullptr,\n"
        "            uint32_t postPassQueryIndex = 0);\n",
        count=1,
        label=f"{path}: optional Beta profiling hook",
    )
    path.write_text(text, encoding="utf-8")


def patch_beta_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "postPassQueryIndex + 4" in text:
        return
    text = replace_exact(
        text,
        "void Beta::Dispatch(const Core::CommandBuffer& buf, uint64_t frameCount) {\n",
        "void Beta::Dispatch(const Core::CommandBuffer& buf, uint64_t frameCount,\n"
        "        Core::TimestampQueryPool* profilePool, uint32_t postPassQueryIndex) {\n",
        count=1,
        label=f"{path}: Beta profiling signature",
    )

    # Each timestamp is inserted immediately after the corresponding dispatch.
    # The five writes remain in the same command buffer and add no queue edges.
    anchors = (
        "    this->firstDescriptorSet.at(frameCount % 3).bind(buf, this->pipelines.at(0));\n"
        "    buf.dispatch(threadsX, threadsY, 1);\n",
        "    this->descriptorSets.at(0).bind(buf, this->pipelines.at(1));\n"
        "    buf.dispatch(threadsX, threadsY, 1);\n",
        "    this->descriptorSets.at(1).bind(buf, this->pipelines.at(2));\n"
        "    buf.dispatch(threadsX, threadsY, 1);\n",
        "    this->descriptorSets.at(2).bind(buf, this->pipelines.at(3));\n"
        "    buf.dispatch(threadsX, threadsY, 1);\n",
        "    this->descriptorSets.at(3).bind(buf, this->pipelines.at(4));\n"
        "    buf.dispatch(threadsX, threadsY, 1);\n",
    )
    for index, anchor in enumerate(anchors):
        profiled = (
            anchor
            + "    if (profilePool != nullptr && profilePool->supported())\n"
            + "        profilePool->write(buf.handle(), postPassQueryIndex + "
            + str(index)
            + ");\n"
        )
        text = replace_exact(
            text, anchor, profiled, count=1,
            label=f"{path}: Beta pass {index} timestamp",
        )
    path.write_text(text, encoding="utf-8")


def patch_framegen_header(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    text = replace_exact(
        text,
        "        std::array<double, 4> generatedPreProfileTotalsMs{};\n",
        "        std::array<double, 8> generatedPreProfileTotalsMs{};\n",
        count=1,
        label=f"{path}: five-pass Beta profile accumulator",
    )
    path.write_text(text, encoding="utf-8")


def patch_framegen_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "beta4_avg_ms=" in text:
        return

    text = replace_exact(
        text,
        "        data.generatedPreQueryPool = Core::TimestampQueryPool(vk.device, 5);\n",
        "        data.generatedPreQueryPool = Core::TimestampQueryPool(vk.device, 9);\n",
        count=2,
        label=f"{path}: generated pre-stage query capacity",
    )

    old_beta_dispatch = (
        "    if (generationCount > 0) {\n"
        "        this->beta.Dispatch(data.cmdBuffer1, this->frameIdx);\n"
        "        if (profileGenerated)\n"
        "            data.generatedPreQueryPool.write(data.cmdBuffer1.handle(), 4);\n"
        "    }\n"
    )
    new_beta_dispatch = (
        "    if (generationCount > 0) {\n"
        "        this->beta.Dispatch(\n"
        "            data.cmdBuffer1, this->frameIdx,\n"
        "            profileGenerated ? &data.generatedPreQueryPool : nullptr, 4);\n"
        "    }\n"
    )
    text = replace_exact(
        text, old_beta_dispatch, new_beta_dispatch, count=1,
        label=f"{path}: Beta per-pass profiling dispatch",
    )

    old_beta_log = (
        '                << " beta_avg_ms=" << (this->generatedPreProfileTotalsMs.at(3) / sourceSamples)\n'
    )
    new_beta_log = (
        '                << " beta_avg_ms=" << ((this->generatedPreProfileTotalsMs.at(3)\n'
        '                    + this->generatedPreProfileTotalsMs.at(4)\n'
        '                    + this->generatedPreProfileTotalsMs.at(5)\n'
        '                    + this->generatedPreProfileTotalsMs.at(6)\n'
        '                    + this->generatedPreProfileTotalsMs.at(7)) / sourceSamples)\n'
        '                << " beta0_avg_ms=" << (this->generatedPreProfileTotalsMs.at(3) / sourceSamples)\n'
        '                << " beta1_avg_ms=" << (this->generatedPreProfileTotalsMs.at(4) / sourceSamples)\n'
        '                << " beta2_avg_ms=" << (this->generatedPreProfileTotalsMs.at(5) / sourceSamples)\n'
        '                << " beta3_avg_ms=" << (this->generatedPreProfileTotalsMs.at(6) / sourceSamples)\n'
        '                << " beta4_avg_ms=" << (this->generatedPreProfileTotalsMs.at(7) / sourceSamples)\n'
    )
    text = replace_exact(
        text, old_beta_log, new_beta_log, count=1,
        label=f"{path}: Beta per-pass profile log",
    )
    path.write_text(text, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    root = args.root.resolve()

    patch_translation_source(root / TRANSLATION_SOURCE)
    for header, source in BETA_PATHS:
        patch_beta_header(root / header)
        patch_beta_source(root / source)
    for rel in FRAMEGEN_HEADERS:
        patch_framegen_header(root / rel)
    for rel in FRAMEGEN_SOURCES:
        patch_framegen_source(root / rel)


if __name__ == "__main__":
    main()
