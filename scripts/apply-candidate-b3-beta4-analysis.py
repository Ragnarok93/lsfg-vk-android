#!/usr/bin/env python3
"""Add read-only Beta 4 barrier/shared-memory and raw SPIR-V evidence.

Candidate B3 deliberately does not mutate translated SPIR-V. It runs after the
Candidate B2 Mipmaps dependency probe and reuses the already-injected SPIR-V
helpers to profile p_beta[4] only. The raw module is dumped once per process so
its exact def-use/control-flow can be reconstructed offline before any shader
rewrite is attempted.
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


def beta4_helper() -> str:
    return r'''    void logBeta4DependencyProfile(
            const std::string& shaderName,
            const std::vector<uint8_t>& bytecode) {
        if (shaderName != "p_beta[4]")
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
            std::cerr << "lsfg-vk: beta4-phase-profile"
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
                const uint64_t executionScope = b2ConstantValue(
                    constants, readSpirvWord(bytecode, cursor + 1));
                const uint64_t memoryScope = b2ConstantValue(
                    constants, readSpirvWord(bytecode, cursor + 2));
                const uint64_t memorySemantics = b2ConstantValue(
                    constants, readSpirvWord(bytecode, cursor + 3));
                std::ostringstream semanticsHex;
                semanticsHex << std::hex << memorySemantics;

                std::cerr << "lsfg-vk: beta4-barrier-profile"
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

        static bool dumpedPerformanceBeta4 = false;
        if (dumpedPerformanceBeta4)
            return;
        dumpedPerformanceBeta4 = true;

        const size_t chunkCount =
            (bytecode.size() + kSpirvDumpBytesPerChunk - 1) / kSpirvDumpBytesPerChunk;
        for (size_t chunk = 0; chunk < chunkCount; ++chunk) {
            const size_t begin = chunk * kSpirvDumpBytesPerChunk;
            const size_t end = std::min(begin + kSpirvDumpBytesPerChunk, bytecode.size());
            std::ostringstream hex;
            hex << std::hex << std::setfill('0');
            for (size_t i = begin; i < end; ++i)
                hex << std::setw(2) << static_cast<unsigned int>(bytecode.at(i));
            std::cerr << "lsfg-vk: beta4-spirv-chunk"
                << " shader=" << shaderName
                << " chunk=" << (chunk + 1)
                << " chunks=" << chunkCount
                << " hex=" << hex.str()
                << std::endl;
        }
    }

'''


def patch_translation_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "beta4-barrier-profile" in text:
        return

    if "mipmaps-barrier-profile" not in text or "isB2WorkgroupPointerProducer" not in text:
        raise RuntimeError(f"{path}: Candidate B3 requires the Candidate B2 dependency probe")

    text = replace_exact(
        text,
        "    void logMipmapsSpirvProfile(\n",
        beta4_helper() + "    void logMipmapsSpirvProfile(\n",
        count=1,
        label=f"{path}: Beta 4 dependency helper",
    )
    text = replace_exact(
        text,
        "    logMipmapsSpirvProfile(shaderName, spirvBytecode);\n",
        "    logMipmapsSpirvProfile(shaderName, spirvBytecode);\n"
        "    logBeta4DependencyProfile(shaderName, spirvBytecode);\n",
        count=1,
        label=f"{path}: Beta 4 dependency log invocation",
    )
    path.write_text(text, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    patch_translation_source(args.root.resolve() / TRANSLATION_SOURCE)


if __name__ == "__main__":
    main()
