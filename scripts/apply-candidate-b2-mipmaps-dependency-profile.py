#!/usr/bin/env python3
"""Add compact Mipmaps barrier/shared-memory dependency evidence to Android profiling builds.

Candidate B2 deliberately does not mutate SPIR-V. It runs after the existing
Mipmaps/Beta profiler and annotates the translated Mipmaps module with:
  * each OpControlBarrier's execution scope, memory scope, and semantics;
  * linear workgroup-memory/image traffic in the phase preceding each barrier;
  * the final phase after the last barrier.

The goal is to prove which internal barriers actually protect TGSM traffic before
attempting any shader rewrite on Adreno 6xx.
"""
from __future__ import annotations

import argparse
from pathlib import Path


TRANSLATION_SOURCE = Path("src/extract/trans.cpp")


def replace_exact(text: str, old: str, new: str, *, count: int, label: str) -> str:
    if new in text:
        return text
    found = text.count(old)
    if found != count:
        raise RuntimeError(f"{label}: expected {count} match(es), found {found}")
    return text.replace(old, new, count)


def dependency_helper() -> str:
    return r'''    constexpr uint16_t kB2OpLoad = static_cast<uint16_t>(spv::OpLoad);
    constexpr uint16_t kB2OpStore = static_cast<uint16_t>(spv::OpStore);
    constexpr uint16_t kB2OpAccessChain = static_cast<uint16_t>(spv::OpAccessChain);
    constexpr uint16_t kB2OpInBoundsAccessChain = static_cast<uint16_t>(spv::OpInBoundsAccessChain);
    constexpr uint16_t kB2OpPtrAccessChain = static_cast<uint16_t>(spv::OpPtrAccessChain);
    constexpr uint16_t kB2OpCopyObject = static_cast<uint16_t>(spv::OpCopyObject);
    constexpr uint16_t kB2OpPhi = static_cast<uint16_t>(spv::OpPhi);
    constexpr uint16_t kB2OpSelect = static_cast<uint16_t>(spv::OpSelect);

    bool isB2WorkgroupPointerProducer(uint16_t opCode) {
        switch (opCode) {
        case kB2OpAccessChain:
        case kB2OpInBoundsAccessChain:
        case kB2OpPtrAccessChain:
        case kB2OpCopyObject:
        case kB2OpPhi:
        case kB2OpSelect:
            return true;
        default:
            return false;
        }
    }

    uint64_t b2ConstantValue(
            const std::unordered_map<uint32_t, uint64_t>& constants,
            uint32_t id) {
        const auto it = constants.find(id);
        return it == constants.end() ? std::numeric_limits<uint64_t>::max() : it->second;
    }

    void logMipmapsBarrierDependencyProfile(
            const std::string& shaderName,
            const std::vector<uint8_t>& bytecode) {
        if (shaderName != "mipmaps" && shaderName != "p_mipmaps")
            return;

        const size_t wordCount = bytecode.size() / sizeof(uint32_t);
        if (bytecode.size() % sizeof(uint32_t) != 0
                || wordCount < 5
                || readSpirvWord(bytecode, 0) != kSpirvMagic)
            return;

        std::unordered_map<uint32_t, uint64_t> constants;
        std::unordered_set<uint32_t> workgroupPointerTypes;
        size_t cursor = 5;
        while (cursor < wordCount) {
            const uint32_t firstWord = readSpirvWord(bytecode, cursor);
            const uint16_t instructionWordCount = static_cast<uint16_t>(firstWord >> 16U);
            const uint16_t opCode = static_cast<uint16_t>(firstWord & 0xffffU);
            if (instructionWordCount == 0 || cursor + instructionWordCount > wordCount)
                return;

            if (opCode == kOpTypePointer && instructionWordCount >= 4) {
                const uint32_t resultId = readSpirvWord(bytecode, cursor + 1);
                const uint32_t storageClass = readSpirvWord(bytecode, cursor + 2);
                if (storageClass == kStorageClassWorkgroup)
                    workgroupPointerTypes.insert(resultId);
            } else if (opCode == kOpConstant && instructionWordCount >= 4) {
                constants[readSpirvWord(bytecode, cursor + 2)] =
                    readSpirvWord(bytecode, cursor + 3);
            }
            cursor += instructionWordCount;
        }

        std::unordered_set<uint32_t> workgroupPointers;
        size_t phase = 0;
        size_t barrierIndex = 0;
        size_t workgroupLoads = 0;
        size_t workgroupStores = 0;
        size_t imageSamples = 0;
        size_t imageWrites = 0;

        const auto logPhase = [&](bool finalPhase) {
            std::cerr << "lsfg-vk: mipmaps-phase-profile"
                << " shader=" << shaderName
                << " phase=" << phase
                << " wg_loads=" << workgroupLoads
                << " wg_stores=" << workgroupStores
                << " image_samples=" << imageSamples
                << " image_writes=" << imageWrites
                << " final=" << (finalPhase ? 1 : 0)
                << std::endl;
        };

        cursor = 5;
        while (cursor < wordCount) {
            const uint32_t firstWord = readSpirvWord(bytecode, cursor);
            const uint16_t instructionWordCount = static_cast<uint16_t>(firstWord >> 16U);
            const uint16_t opCode = static_cast<uint16_t>(firstWord & 0xffffU);
            if (instructionWordCount == 0 || cursor + instructionWordCount > wordCount)
                return;

            if (opCode == kOpVariable && instructionWordCount >= 4) {
                const uint32_t resultType = readSpirvWord(bytecode, cursor + 1);
                const uint32_t resultId = readSpirvWord(bytecode, cursor + 2);
                const uint32_t storageClass = readSpirvWord(bytecode, cursor + 3);
                if (storageClass == kStorageClassWorkgroup
                        || workgroupPointerTypes.count(resultType) != 0)
                    workgroupPointers.insert(resultId);
            } else if (isB2WorkgroupPointerProducer(opCode) && instructionWordCount >= 3) {
                const uint32_t resultType = readSpirvWord(bytecode, cursor + 1);
                const uint32_t resultId = readSpirvWord(bytecode, cursor + 2);
                if (workgroupPointerTypes.count(resultType) != 0)
                    workgroupPointers.insert(resultId);
            }

            if (opCode == kB2OpLoad && instructionWordCount >= 4) {
                const uint32_t pointerId = readSpirvWord(bytecode, cursor + 3);
                if (workgroupPointers.count(pointerId) != 0)
                    ++workgroupLoads;
            } else if (opCode == kB2OpStore && instructionWordCount >= 3) {
                const uint32_t pointerId = readSpirvWord(bytecode, cursor + 1);
                if (workgroupPointers.count(pointerId) != 0)
                    ++workgroupStores;
            }

            if (isImageSampleOp(opCode))
                ++imageSamples;
            else if (opCode == kOpImageWrite)
                ++imageWrites;

            if (opCode == kOpControlBarrier && instructionWordCount >= 4) {
                const uint32_t executionScopeId = readSpirvWord(bytecode, cursor + 1);
                const uint32_t memoryScopeId = readSpirvWord(bytecode, cursor + 2);
                const uint32_t memorySemanticsId = readSpirvWord(bytecode, cursor + 3);
                const uint64_t executionScope = b2ConstantValue(constants, executionScopeId);
                const uint64_t memoryScope = b2ConstantValue(constants, memoryScopeId);
                const uint64_t memorySemantics = b2ConstantValue(constants, memorySemanticsId);
                std::ostringstream semanticsHex;
                semanticsHex << std::hex << memorySemantics;

                std::cerr << "lsfg-vk: mipmaps-barrier-profile"
                    << " shader=" << shaderName
                    << " barrier=" << barrierIndex
                    << " phase_before=" << phase
                    << " execution_scope=" << executionScope
                    << " memory_scope=" << memoryScope
                    << " memory_semantics=0x" << semanticsHex.str()
                    << " pre_wg_loads=" << workgroupLoads
                    << " pre_wg_stores=" << workgroupStores
                    << " pre_image_samples=" << imageSamples
                    << " pre_image_writes=" << imageWrites
                    << std::endl;
                logPhase(false);

                ++barrierIndex;
                ++phase;
                workgroupLoads = 0;
                workgroupStores = 0;
                imageSamples = 0;
                imageWrites = 0;
            }

            cursor += instructionWordCount;
        }

        logPhase(true);
    }

'''


def patch_translation_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "mipmaps-barrier-profile" in text:
        return

    if "void logMipmapsSpirvProfile(" not in text:
        raise RuntimeError(
            f"{path}: Candidate B2 requires the existing Mipmaps structure profiler"
        )

    text = replace_exact(
        text,
        "    void logMipmapsSpirvProfile(\n",
        dependency_helper() + "    void logMipmapsSpirvProfile(\n",
        count=1,
        label=f"{path}: Mipmaps dependency helper",
    )
    text = replace_exact(
        text,
        "        logMipmapsSpirvDump(shaderName, bytecode);\n",
        "        logMipmapsBarrierDependencyProfile(shaderName, bytecode);\n"
        "        logMipmapsSpirvDump(shaderName, bytecode);\n",
        count=1,
        label=f"{path}: Mipmaps dependency log invocation",
    )
    path.write_text(text, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    patch_translation_source(args.root.resolve() / TRANSLATION_SOURCE)


if __name__ == "__main__":
    main()
