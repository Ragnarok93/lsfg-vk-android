#!/usr/bin/env python3
"""Apply Candidate B6's exact Mipmaps phase-0 private-register SSA cleanup.

The Performance p_mipmaps translator output begins with a straight-line phase
before its first workgroup barrier. B6 forwards direct Private-register loads
from earlier stores in that straight-line phase and drops stores whose values
cannot escape the phase. The exact B4-era module is fingerprinted before the
rewrite and the exact expected output is fingerprinted before installation.
Sampling, image writes, shared memory, barriers, control flow, precision,
formats and local size are unchanged; any mismatch leaves the module untouched.
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


def helper() -> str:
    return r'''namespace {
    constexpr uint64_t kB6ExpectedFnv1a64 = 0x65d3c6a69e9f9b07ULL;
    constexpr size_t kB6ExpectedBytes = 28832;
    constexpr uint32_t kB6ExpectedBound = 1270;
    constexpr uint64_t kB6ExpectedOutputFnv1a64 = 0x56d1ebba7cb3ac76ULL;
    constexpr size_t kB6ExpectedOutputBytes = 28052;
    constexpr size_t kB6ExpectedPrivateVariables = 9;
    constexpr size_t kB6ExpectedPromotedLoads = 143;
    constexpr size_t kB6ExpectedRemovedStores = 65;
    constexpr size_t kB6ExpectedLiveOutVariables = 1;

    constexpr uint16_t kB6OpFunction = static_cast<uint16_t>(spv::OpFunction);
    constexpr uint16_t kB6OpLabel = static_cast<uint16_t>(spv::OpLabel);
    constexpr uint16_t kB6OpVariable = static_cast<uint16_t>(spv::OpVariable);
    constexpr uint16_t kB6OpLoad = static_cast<uint16_t>(spv::OpLoad);
    constexpr uint16_t kB6OpStore = static_cast<uint16_t>(spv::OpStore);
    constexpr uint16_t kB6OpCopyObject = static_cast<uint16_t>(spv::OpCopyObject);
    constexpr uint16_t kB6OpAccessChain = static_cast<uint16_t>(spv::OpAccessChain);
    constexpr uint16_t kB6OpInBoundsAccessChain = static_cast<uint16_t>(spv::OpInBoundsAccessChain);
    constexpr uint16_t kB6OpControlBarrier = static_cast<uint16_t>(spv::OpControlBarrier);
    constexpr uint16_t kB6OpSelectionMerge = static_cast<uint16_t>(spv::OpSelectionMerge);
    constexpr uint16_t kB6OpLoopMerge = static_cast<uint16_t>(spv::OpLoopMerge);
    constexpr uint16_t kB6OpBranch = static_cast<uint16_t>(spv::OpBranch);
    constexpr uint16_t kB6OpBranchConditional = static_cast<uint16_t>(spv::OpBranchConditional);
    constexpr uint16_t kB6OpSwitch = static_cast<uint16_t>(spv::OpSwitch);

    uint32_t b6ReadWord(const std::vector<uint8_t>& bytecode, size_t wordIndex) {
        uint32_t word = 0;
        std::memcpy(&word, bytecode.data() + wordIndex * sizeof(uint32_t), sizeof(uint32_t));
        return word;
    }

    uint64_t b6Fnv1a64(const std::vector<uint8_t>& bytecode) {
        uint64_t hash = 1469598103934665603ULL;
        for (const uint8_t value : bytecode) {
            hash ^= static_cast<uint64_t>(value);
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    bool applyCandidateB6MipmapsPhase0Ssa(
            const std::string& shaderName,
            std::vector<uint8_t>& bytecode) {
        if (shaderName != "p_mipmaps")
            return false;

        if (bytecode.size() != kB6ExpectedBytes || b6Fnv1a64(bytecode) != kB6ExpectedFnv1a64) {
            std::cerr << "lsfg-vk: candidate-b6-mipmaps-phase0-ssa shader=" << shaderName
                << " applied=0 reason=fingerprint bytes=" << bytecode.size() << std::endl;
            return false;
        }
        if (bytecode.size() % sizeof(uint32_t) != 0 || bytecode.size() < 5 * sizeof(uint32_t)
                || b6ReadWord(bytecode, 0) != 0x07230203U
                || b6ReadWord(bytecode, 3) != kB6ExpectedBound) {
            std::cerr << "lsfg-vk: candidate-b6-mipmaps-phase0-ssa shader=" << shaderName
                << " applied=0 reason=header" << std::endl;
            return false;
        }

        const size_t wordCount = bytecode.size() / sizeof(uint32_t);
        std::vector<uint8_t> privateId(kB6ExpectedBound, 0U);
        size_t privateCount = 0;
        size_t firstLabelEnd = 0;
        size_t firstBarrier = 0;
        bool inFunction = false;

        for (size_t cursor = 5; cursor < wordCount;) {
            const uint32_t firstWord = b6ReadWord(bytecode, cursor);
            const uint16_t instructionWordCount = static_cast<uint16_t>(firstWord >> 16U);
            const uint16_t opCode = static_cast<uint16_t>(firstWord & 0xffffU);
            if (instructionWordCount == 0 || cursor + instructionWordCount > wordCount)
                return false;

            if (!inFunction && opCode == kB6OpVariable && instructionWordCount >= 4
                    && b6ReadWord(bytecode, cursor + 3)
                        == static_cast<uint32_t>(spv::StorageClassPrivate)) {
                const uint32_t resultId = b6ReadWord(bytecode, cursor + 2);
                if (resultId >= kB6ExpectedBound || privateId[resultId] != 0U)
                    return false;
                privateId[resultId] = 1U;
                ++privateCount;
            }
            if (opCode == kB6OpFunction)
                inFunction = true;
            else if (inFunction && firstLabelEnd == 0 && opCode == kB6OpLabel)
                firstLabelEnd = cursor + instructionWordCount;
            else if (firstLabelEnd != 0 && opCode == kB6OpControlBarrier) {
                firstBarrier = cursor;
                break;
            }
            cursor += instructionWordCount;
        }

        if (privateCount != kB6ExpectedPrivateVariables || firstLabelEnd == 0
                || firstBarrier <= firstLabelEnd) {
            std::cerr << "lsfg-vk: candidate-b6-mipmaps-phase0-ssa shader=" << shaderName
                << " applied=0 reason=structure private_vars=" << privateCount << std::endl;
            return false;
        }

        // Phase 0 must remain a single straight-line block. Also reject any
        // derived pointer into a Private register so direct load/store tracking
        // is a complete alias model for this exact phase.
        for (size_t cursor = firstLabelEnd; cursor < firstBarrier;) {
            const uint32_t firstWord = b6ReadWord(bytecode, cursor);
            const uint16_t instructionWordCount = static_cast<uint16_t>(firstWord >> 16U);
            const uint16_t opCode = static_cast<uint16_t>(firstWord & 0xffffU);
            if (instructionWordCount == 0 || cursor + instructionWordCount > firstBarrier)
                return false;
            if (opCode == kB6OpSelectionMerge || opCode == kB6OpLoopMerge
                    || opCode == kB6OpBranch || opCode == kB6OpBranchConditional
                    || opCode == kB6OpSwitch) {
                std::cerr << "lsfg-vk: candidate-b6-mipmaps-phase0-ssa shader=" << shaderName
                    << " applied=0 reason=phase0-control-flow" << std::endl;
                return false;
            }
            if ((opCode == kB6OpAccessChain || opCode == kB6OpInBoundsAccessChain)
                    && instructionWordCount >= 4) {
                const uint32_t baseId = b6ReadWord(bytecode, cursor + 3);
                if (baseId < privateId.size() && privateId[baseId] != 0U) {
                    std::cerr << "lsfg-vk: candidate-b6-mipmaps-phase0-ssa shader=" << shaderName
                        << " applied=0 reason=phase0-private-alias" << std::endl;
                    return false;
                }
            }
            cursor += instructionWordCount;
        }

        // Determine which phase-0 register values are actually live across the
        // first barrier. A first Store after the barrier kills the phase-0 value;
        // a first Load means the final phase-0 Store must remain.
        std::vector<uint8_t> firstAccess(kB6ExpectedBound, 0U);
        std::vector<uint8_t> liveOut(kB6ExpectedBound, 0U);
        size_t liveOutCount = 0;
        for (size_t cursor = firstBarrier + 4; cursor < wordCount;) {
            const uint32_t firstWord = b6ReadWord(bytecode, cursor);
            const uint16_t instructionWordCount = static_cast<uint16_t>(firstWord >> 16U);
            const uint16_t opCode = static_cast<uint16_t>(firstWord & 0xffffU);
            if (instructionWordCount == 0 || cursor + instructionWordCount > wordCount)
                return false;
            if (opCode == kB6OpLoad && instructionWordCount == 4) {
                const uint32_t pointerId = b6ReadWord(bytecode, cursor + 3);
                if (pointerId < privateId.size() && privateId[pointerId] != 0U
                        && firstAccess[pointerId] == 0U) {
                    firstAccess[pointerId] = 1U;
                    liveOut[pointerId] = 1U;
                    ++liveOutCount;
                }
            } else if (opCode == kB6OpStore && instructionWordCount == 3) {
                const uint32_t pointerId = b6ReadWord(bytecode, cursor + 1);
                if (pointerId < privateId.size() && privateId[pointerId] != 0U
                        && firstAccess[pointerId] == 0U)
                    firstAccess[pointerId] = 2U;
            }
            cursor += instructionWordCount;
        }
        if (liveOutCount != kB6ExpectedLiveOutVariables) {
            std::cerr << "lsfg-vk: candidate-b6-mipmaps-phase0-ssa shader=" << shaderName
                << " applied=0 reason=live-out-count live_out=" << liveOutCount << std::endl;
            return false;
        }

        std::vector<size_t> finalStore(kB6ExpectedBound, wordCount);
        for (size_t cursor = firstLabelEnd; cursor < firstBarrier;) {
            const uint32_t firstWord = b6ReadWord(bytecode, cursor);
            const uint16_t instructionWordCount = static_cast<uint16_t>(firstWord >> 16U);
            const uint16_t opCode = static_cast<uint16_t>(firstWord & 0xffffU);
            if (opCode == kB6OpStore && instructionWordCount == 3) {
                const uint32_t pointerId = b6ReadWord(bytecode, cursor + 1);
                if (pointerId < privateId.size() && liveOut[pointerId] != 0U)
                    finalStore[pointerId] = cursor;
            }
            cursor += instructionWordCount;
        }
        for (size_t id = 0; id < liveOut.size(); ++id)
            if (liveOut[id] != 0U && finalStore[id] == wordCount)
                return false;

        std::vector<uint32_t> currentValue(kB6ExpectedBound, 0U);
        std::vector<uint32_t> rewritten;
        rewritten.reserve(wordCount);
        for (size_t i = 0; i < 5; ++i)
            rewritten.push_back(b6ReadWord(bytecode, i));

        size_t promotedLoads = 0;
        size_t removedStores = 0;
        for (size_t cursor = 5; cursor < wordCount;) {
            const uint32_t firstWord = b6ReadWord(bytecode, cursor);
            const uint16_t instructionWordCount = static_cast<uint16_t>(firstWord >> 16U);
            const uint16_t opCode = static_cast<uint16_t>(firstWord & 0xffffU);

            if (cursor >= firstLabelEnd && cursor < firstBarrier
                    && opCode == kB6OpStore && instructionWordCount == 3) {
                const uint32_t pointerId = b6ReadWord(bytecode, cursor + 1);
                if (pointerId < privateId.size() && privateId[pointerId] != 0U) {
                    currentValue[pointerId] = b6ReadWord(bytecode, cursor + 2);
                    if (liveOut[pointerId] == 0U || finalStore[pointerId] != cursor) {
                        ++removedStores;
                        cursor += instructionWordCount;
                        continue;
                    }
                }
            }

            if (cursor >= firstLabelEnd && cursor < firstBarrier
                    && opCode == kB6OpLoad && instructionWordCount == 4) {
                const uint32_t pointerId = b6ReadWord(bytecode, cursor + 3);
                if (pointerId < privateId.size() && privateId[pointerId] != 0U
                        && currentValue[pointerId] != 0U) {
                    rewritten.push_back((4U << 16U) | static_cast<uint32_t>(kB6OpCopyObject));
                    rewritten.push_back(b6ReadWord(bytecode, cursor + 1));
                    rewritten.push_back(b6ReadWord(bytecode, cursor + 2));
                    rewritten.push_back(currentValue[pointerId]);
                    ++promotedLoads;
                    cursor += instructionWordCount;
                    continue;
                }
            }

            for (size_t i = 0; i < instructionWordCount; ++i)
                rewritten.push_back(b6ReadWord(bytecode, cursor + i));
            cursor += instructionWordCount;
        }

        if (promotedLoads != kB6ExpectedPromotedLoads
                || removedStores != kB6ExpectedRemovedStores) {
            std::cerr << "lsfg-vk: candidate-b6-mipmaps-phase0-ssa shader=" << shaderName
                << " applied=0 reason=rewrite-count promoted_loads=" << promotedLoads
                << " removed_stores=" << removedStores << std::endl;
            return false;
        }

        std::vector<uint8_t> candidate(rewritten.size() * sizeof(uint32_t));
        std::memcpy(candidate.data(), rewritten.data(), candidate.size());
        if (candidate.size() != kB6ExpectedOutputBytes
                || b6ReadWord(candidate, 3) != kB6ExpectedBound
                || b6Fnv1a64(candidate) != kB6ExpectedOutputFnv1a64) {
            std::cerr << "lsfg-vk: candidate-b6-mipmaps-phase0-ssa shader=" << shaderName
                << " applied=0 reason=output-fingerprint bytes=" << candidate.size() << std::endl;
            return false;
        }

        bytecode = std::move(candidate);
        std::cerr << "lsfg-vk: candidate-b6-mipmaps-phase0-ssa shader=" << shaderName
            << " applied=1 promoted_loads=" << promotedLoads
            << " removed_stores=" << removedStores
            << " private_vars=" << privateCount << " live_out=" << liveOutCount
            << " old_words=" << wordCount << " new_words=" << rewritten.size() << std::endl;
        return true;
    }
}

'''


def patch_translation_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "candidate-b6-mipmaps-phase0-ssa" in text:
        return
    if "#include <cstring>" not in text:
        text = replace_exact(text, "#include <cstddef>\n", "#include <cstddef>\n#include <cstring>\n",
            count=1, label=f"{path}: cstring include")
    if "#include <iostream>" not in text:
        text = replace_exact(text, "#include <cstdint>\n", "#include <cstdint>\n#include <iostream>\n",
            count=1, label=f"{path}: iostream include")
    text = replace_exact(text, "struct BindingOffsets {\n", helper() + "struct BindingOffsets {\n",
        count=1, label=f"{path}: Candidate B6 helper")

    b4_anchor = "    applyCandidateB4Beta4PredicateCanonicalization(shaderName, spirvBytecode);\n"
    text = replace_exact(text, b4_anchor,
        b4_anchor + "    applyCandidateB6MipmapsPhase0Ssa(shaderName, spirvBytecode);\n",
        count=1, label=f"{path}: Candidate B6 invocation")
    path.write_text(text, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    patch_translation_source(args.root.resolve() / TRANSLATION_SOURCE)


if __name__ == "__main__":
    main()
