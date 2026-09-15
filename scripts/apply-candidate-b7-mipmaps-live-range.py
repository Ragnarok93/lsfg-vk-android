#!/usr/bin/env python3
"""Apply Candidate B7's exact Mipmaps phase-0 live-range reduction.

B7 first performs the exact, lossless phase-0 private-register SSA cleanup on
the retained-B4 Performance p_mipmaps module. It then reorders only independent
phase-0 instructions so each of the four source samples is consumed by its
transfer chain before the next sample is issued. Three condition-only carrier
uses are redirected to a dedicated equivalent carrier. Sampling order, image
writes, shared-memory reduction, barriers, precision, formats, local size and
the floating-point expression graph are unchanged. Exact input, intermediate
and output fingerprints make translator drift a safe no-op.
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
    constexpr uint64_t kB7ExpectedInputFnv1a64 = 0x65d3c6a69e9f9b07ULL;
    constexpr size_t kB7ExpectedInputBytes = 28832;
    constexpr uint32_t kB7ExpectedInputBound = 1270;
    constexpr uint64_t kB7ExpectedSsaFnv1a64 = 0x56d1ebba7cb3ac76ULL;
    constexpr size_t kB7ExpectedSsaBytes = 28052;
    constexpr uint64_t kB7ExpectedOutputFnv1a64 = 0xa8153e454b8da00aULL;
    constexpr size_t kB7ExpectedOutputBytes = 28076;
    constexpr uint32_t kB7OutputBound = 1271;
    constexpr size_t kB7ExpectedPrivateVariables = 9;
    constexpr size_t kB7ExpectedPromotedLoads = 143;
    constexpr size_t kB7ExpectedRemovedStores = 65;
    constexpr size_t kB7ExpectedLiveOutVariables = 1;
    constexpr size_t kB7ExpectedSsaInstructions = 1443;
    constexpr uint32_t kB7FlagCarrierId = 1270;

    constexpr size_t kB7AStart = 295;
    constexpr size_t kB7BStart = 302;
    constexpr size_t kB7CStart = 313;
    constexpr size_t kB7DStart = 324;
    constexpr size_t kB7ConditionStart = 335;
    constexpr size_t kB7ABlockStart = 345;
    constexpr size_t kB7BBlockStart = 423;
    constexpr size_t kB7CBlockStart = 501;
    constexpr size_t kB7DBlockStart = 579;
    constexpr size_t kB7TailStart = 655;
    constexpr size_t kB7PatchedFlagUses[] = {398U, 476U, 554U};

    constexpr uint16_t kB7OpFunction = static_cast<uint16_t>(spv::OpFunction);
    constexpr uint16_t kB7OpLabel = static_cast<uint16_t>(spv::OpLabel);
    constexpr uint16_t kB7OpVariable = static_cast<uint16_t>(spv::OpVariable);
    constexpr uint16_t kB7OpLoad = static_cast<uint16_t>(spv::OpLoad);
    constexpr uint16_t kB7OpStore = static_cast<uint16_t>(spv::OpStore);
    constexpr uint16_t kB7OpCopyObject = static_cast<uint16_t>(spv::OpCopyObject);
    constexpr uint16_t kB7OpCompositeInsert = static_cast<uint16_t>(spv::OpCompositeInsert);
    constexpr uint16_t kB7OpAccessChain = static_cast<uint16_t>(spv::OpAccessChain);
    constexpr uint16_t kB7OpInBoundsAccessChain =
        static_cast<uint16_t>(spv::OpInBoundsAccessChain);
    constexpr uint16_t kB7OpControlBarrier = static_cast<uint16_t>(spv::OpControlBarrier);
    constexpr uint16_t kB7OpSelectionMerge = static_cast<uint16_t>(spv::OpSelectionMerge);
    constexpr uint16_t kB7OpLoopMerge = static_cast<uint16_t>(spv::OpLoopMerge);
    constexpr uint16_t kB7OpBranch = static_cast<uint16_t>(spv::OpBranch);
    constexpr uint16_t kB7OpBranchConditional =
        static_cast<uint16_t>(spv::OpBranchConditional);
    constexpr uint16_t kB7OpSwitch = static_cast<uint16_t>(spv::OpSwitch);

    struct B7Instruction {
        size_t word = 0;
        uint16_t wordCount = 0;
        uint16_t opCode = 0;
    };

    uint32_t b7ReadWord(const std::vector<uint8_t>& bytecode, size_t wordIndex) {
        uint32_t word = 0;
        std::memcpy(&word,
            bytecode.data() + wordIndex * sizeof(uint32_t), sizeof(uint32_t));
        return word;
    }

    uint64_t b7Fnv1a64(const std::vector<uint8_t>& bytecode) {
        uint64_t hash = 1469598103934665603ULL;
        for (const uint8_t value : bytecode) {
            hash ^= static_cast<uint64_t>(value);
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    bool b7BuildPhase0Ssa(const std::vector<uint8_t>& bytecode,
            std::vector<uint8_t>& output, size_t& promotedLoads,
            size_t& removedStores) {
        if (bytecode.size() != kB7ExpectedInputBytes
                || b7Fnv1a64(bytecode) != kB7ExpectedInputFnv1a64
                || bytecode.size() % sizeof(uint32_t) != 0
                || b7ReadWord(bytecode, 0) != 0x07230203U
                || b7ReadWord(bytecode, 3) != kB7ExpectedInputBound)
            return false;

        const size_t wordCount = bytecode.size() / sizeof(uint32_t);
        std::vector<uint8_t> privateId(kB7ExpectedInputBound, 0U);
        size_t privateCount = 0;
        size_t firstLabelEnd = 0;
        size_t firstBarrier = 0;
        bool inFunction = false;
        for (size_t cursor = 5; cursor < wordCount;) {
            const uint32_t firstWord = b7ReadWord(bytecode, cursor);
            const uint16_t instructionWordCount = static_cast<uint16_t>(firstWord >> 16U);
            const uint16_t opCode = static_cast<uint16_t>(firstWord & 0xffffU);
            if (instructionWordCount == 0 || cursor + instructionWordCount > wordCount)
                return false;
            if (!inFunction && opCode == kB7OpVariable && instructionWordCount >= 4
                    && b7ReadWord(bytecode, cursor + 3)
                        == static_cast<uint32_t>(spv::StorageClassPrivate)) {
                const uint32_t resultId = b7ReadWord(bytecode, cursor + 2);
                if (resultId >= kB7ExpectedInputBound || privateId[resultId] != 0U)
                    return false;
                privateId[resultId] = 1U;
                ++privateCount;
            }
            if (opCode == kB7OpFunction)
                inFunction = true;
            else if (inFunction && firstLabelEnd == 0 && opCode == kB7OpLabel)
                firstLabelEnd = cursor + instructionWordCount;
            else if (firstLabelEnd != 0 && opCode == kB7OpControlBarrier) {
                firstBarrier = cursor;
                break;
            }
            cursor += instructionWordCount;
        }
        if (privateCount != kB7ExpectedPrivateVariables
                || firstLabelEnd == 0 || firstBarrier <= firstLabelEnd)
            return false;

        for (size_t cursor = firstLabelEnd; cursor < firstBarrier;) {
            const uint32_t firstWord = b7ReadWord(bytecode, cursor);
            const uint16_t instructionWordCount = static_cast<uint16_t>(firstWord >> 16U);
            const uint16_t opCode = static_cast<uint16_t>(firstWord & 0xffffU);
            if (instructionWordCount == 0 || cursor + instructionWordCount > firstBarrier)
                return false;
            if (opCode == kB7OpSelectionMerge || opCode == kB7OpLoopMerge
                    || opCode == kB7OpBranch || opCode == kB7OpBranchConditional
                    || opCode == kB7OpSwitch)
                return false;
            if ((opCode == kB7OpAccessChain || opCode == kB7OpInBoundsAccessChain)
                    && instructionWordCount >= 4) {
                const uint32_t baseId = b7ReadWord(bytecode, cursor + 3);
                if (baseId < privateId.size() && privateId[baseId] != 0U)
                    return false;
            }
            cursor += instructionWordCount;
        }

        std::vector<uint8_t> firstAccess(kB7ExpectedInputBound, 0U);
        std::vector<uint8_t> liveOut(kB7ExpectedInputBound, 0U);
        size_t liveOutCount = 0;
        for (size_t cursor = firstBarrier + 4; cursor < wordCount;) {
            const uint32_t firstWord = b7ReadWord(bytecode, cursor);
            const uint16_t instructionWordCount = static_cast<uint16_t>(firstWord >> 16U);
            const uint16_t opCode = static_cast<uint16_t>(firstWord & 0xffffU);
            if (instructionWordCount == 0 || cursor + instructionWordCount > wordCount)
                return false;
            if (opCode == kB7OpLoad && instructionWordCount == 4) {
                const uint32_t pointerId = b7ReadWord(bytecode, cursor + 3);
                if (pointerId < privateId.size() && privateId[pointerId] != 0U
                        && firstAccess[pointerId] == 0U) {
                    firstAccess[pointerId] = 1U;
                    liveOut[pointerId] = 1U;
                    ++liveOutCount;
                }
            } else if (opCode == kB7OpStore && instructionWordCount == 3) {
                const uint32_t pointerId = b7ReadWord(bytecode, cursor + 1);
                if (pointerId < privateId.size() && privateId[pointerId] != 0U
                        && firstAccess[pointerId] == 0U)
                    firstAccess[pointerId] = 2U;
            }
            cursor += instructionWordCount;
        }
        if (liveOutCount != kB7ExpectedLiveOutVariables)
            return false;

        std::vector<size_t> finalStore(kB7ExpectedInputBound, wordCount);
        for (size_t cursor = firstLabelEnd; cursor < firstBarrier;) {
            const uint32_t firstWord = b7ReadWord(bytecode, cursor);
            const uint16_t instructionWordCount = static_cast<uint16_t>(firstWord >> 16U);
            const uint16_t opCode = static_cast<uint16_t>(firstWord & 0xffffU);
            if (opCode == kB7OpStore && instructionWordCount == 3) {
                const uint32_t pointerId = b7ReadWord(bytecode, cursor + 1);
                if (pointerId < privateId.size() && liveOut[pointerId] != 0U)
                    finalStore[pointerId] = cursor;
            }
            cursor += instructionWordCount;
        }
        for (size_t id = 0; id < liveOut.size(); ++id)
            if (liveOut[id] != 0U && finalStore[id] == wordCount)
                return false;

        std::vector<uint32_t> currentValue(kB7ExpectedInputBound, 0U);
        std::vector<uint32_t> rewritten;
        rewritten.reserve(wordCount);
        for (size_t i = 0; i < 5; ++i)
            rewritten.push_back(b7ReadWord(bytecode, i));
        promotedLoads = 0;
        removedStores = 0;
        for (size_t cursor = 5; cursor < wordCount;) {
            const uint32_t firstWord = b7ReadWord(bytecode, cursor);
            const uint16_t instructionWordCount = static_cast<uint16_t>(firstWord >> 16U);
            const uint16_t opCode = static_cast<uint16_t>(firstWord & 0xffffU);
            if (cursor >= firstLabelEnd && cursor < firstBarrier
                    && opCode == kB7OpStore && instructionWordCount == 3) {
                const uint32_t pointerId = b7ReadWord(bytecode, cursor + 1);
                if (pointerId < privateId.size() && privateId[pointerId] != 0U) {
                    currentValue[pointerId] = b7ReadWord(bytecode, cursor + 2);
                    if (liveOut[pointerId] == 0U || finalStore[pointerId] != cursor) {
                        ++removedStores;
                        cursor += instructionWordCount;
                        continue;
                    }
                }
            }
            if (cursor >= firstLabelEnd && cursor < firstBarrier
                    && opCode == kB7OpLoad && instructionWordCount == 4) {
                const uint32_t pointerId = b7ReadWord(bytecode, cursor + 3);
                if (pointerId < privateId.size() && privateId[pointerId] != 0U
                        && currentValue[pointerId] != 0U) {
                    rewritten.push_back((4U << 16U) | static_cast<uint32_t>(kB7OpCopyObject));
                    rewritten.push_back(b7ReadWord(bytecode, cursor + 1));
                    rewritten.push_back(b7ReadWord(bytecode, cursor + 2));
                    rewritten.push_back(currentValue[pointerId]);
                    ++promotedLoads;
                    cursor += instructionWordCount;
                    continue;
                }
            }
            for (size_t i = 0; i < instructionWordCount; ++i)
                rewritten.push_back(b7ReadWord(bytecode, cursor + i));
            cursor += instructionWordCount;
        }
        if (promotedLoads != kB7ExpectedPromotedLoads
                || removedStores != kB7ExpectedRemovedStores)
            return false;
        output.resize(rewritten.size() * sizeof(uint32_t));
        std::memcpy(output.data(), rewritten.data(), output.size());
        return output.size() == kB7ExpectedSsaBytes
            && b7ReadWord(output, 3) == kB7ExpectedInputBound
            && b7Fnv1a64(output) == kB7ExpectedSsaFnv1a64;
    }

    bool applyCandidateB7MipmapsLiveRange(const std::string& shaderName,
            std::vector<uint8_t>& bytecode) {
        if (shaderName != "p_mipmaps")
            return false;
        std::vector<uint8_t> ssa;
        size_t promotedLoads = 0;
        size_t removedStores = 0;
        if (!b7BuildPhase0Ssa(bytecode, ssa, promotedLoads, removedStores)) {
            std::cerr << "lsfg-vk: candidate-b7-mipmaps-live-range shader=" << shaderName
                << " applied=0 reason=input-or-ssa-fingerprint bytes="
                << bytecode.size() << std::endl;
            return false;
        }

        const size_t wordCount = ssa.size() / sizeof(uint32_t);
        std::vector<B7Instruction> instructions;
        instructions.reserve(kB7ExpectedSsaInstructions);
        for (size_t cursor = 5; cursor < wordCount;) {
            const uint32_t firstWord = b7ReadWord(ssa, cursor);
            const uint16_t instructionWordCount = static_cast<uint16_t>(firstWord >> 16U);
            const uint16_t opCode = static_cast<uint16_t>(firstWord & 0xffffU);
            if (instructionWordCount == 0 || cursor + instructionWordCount > wordCount)
                return false;
            instructions.push_back({cursor, instructionWordCount, opCode});
            cursor += instructionWordCount;
        }
        if (instructions.size() != kB7ExpectedSsaInstructions)
            return false;
        for (const size_t ordinal : kB7PatchedFlagUses) {
            if (ordinal >= instructions.size())
                return false;
            const auto& instruction = instructions[ordinal];
            if (instruction.opCode != kB7OpCopyObject || instruction.wordCount != 4
                    || b7ReadWord(ssa, instruction.word + 3) != 231U)
                return false;
        }

        std::vector<uint32_t> rewritten;
        rewritten.reserve(wordCount + 6U);
        for (size_t i = 0; i < 5; ++i)
            rewritten.push_back(b7ReadWord(ssa, i));
        rewritten.at(3) = kB7OutputBound;
        std::vector<uint8_t> emitted(instructions.size(), 0U);
        size_t patchedFlagUses = 0;
        auto appendInstruction = [&](size_t ordinal) -> bool {
            if (ordinal >= instructions.size() || emitted[ordinal] != 0U)
                return false;
            const auto& instruction = instructions[ordinal];
            emitted[ordinal] = 1U;
            const bool patchFlag = ordinal == kB7PatchedFlagUses[0]
                || ordinal == kB7PatchedFlagUses[1]
                || ordinal == kB7PatchedFlagUses[2];
            for (size_t i = 0; i < instruction.wordCount; ++i) {
                uint32_t word = b7ReadWord(ssa, instruction.word + i);
                if (patchFlag && i == 3) {
                    word = kB7FlagCarrierId;
                    ++patchedFlagUses;
                }
                rewritten.push_back(word);
            }
            return true;
        };
        auto appendRange = [&](size_t begin, size_t end) -> bool {
            if (begin > end || end > instructions.size())
                return false;
            for (size_t ordinal = begin; ordinal < end; ++ordinal)
                if (!appendInstruction(ordinal))
                    return false;
            return true;
        };

        if (!appendRange(0, kB7AStart) || !appendRange(kB7ConditionStart, 343U))
            return false;
        rewritten.push_back((6U << 16U) | static_cast<uint32_t>(kB7OpCompositeInsert));
        rewritten.push_back(9U);
        rewritten.push_back(kB7FlagCarrierId);
        rewritten.push_back(229U);
        rewritten.push_back(105U);
        rewritten.push_back(3U);

        if (!appendRange(kB7AStart, kB7BStart)
                || !appendRange(kB7ABlockStart, 421U)
                || !appendRange(kB7BStart, kB7CStart)
                || !appendRange(kB7BBlockStart, kB7CBlockStart)
                || !appendRange(kB7CStart, kB7DStart)
                || !appendRange(421U, kB7BBlockStart)
                || !appendRange(kB7CBlockStart, kB7DBlockStart)
                || !appendRange(kB7DStart, kB7ConditionStart)
                || !appendRange(343U, kB7ABlockStart)
                || !appendRange(kB7DBlockStart, kB7TailStart)
                || !appendRange(kB7TailStart, instructions.size()))
            return false;
        for (const uint8_t value : emitted)
            if (value != 1U)
                return false;
        if (patchedFlagUses != 3U)
            return false;

        std::vector<uint8_t> candidate(rewritten.size() * sizeof(uint32_t));
        std::memcpy(candidate.data(), rewritten.data(), candidate.size());
        if (candidate.size() != kB7ExpectedOutputBytes
                || b7ReadWord(candidate, 3) != kB7OutputBound
                || b7Fnv1a64(candidate) != kB7ExpectedOutputFnv1a64) {
            std::cerr << "lsfg-vk: candidate-b7-mipmaps-live-range shader="
                << shaderName << " applied=0 reason=output-fingerprint bytes="
                << candidate.size() << std::endl;
            return false;
        }
        const size_t oldWords = bytecode.size() / sizeof(uint32_t);
        bytecode = std::move(candidate);
        std::cerr << "lsfg-vk: candidate-b7-mipmaps-live-range shader=" << shaderName
            << " applied=1 promoted_loads=" << promotedLoads
            << " removed_stores=" << removedStores
            << " reordered_samples=4 patched_flag_uses=" << patchedFlagUses
            << " old_words=" << oldWords << " new_words=" << rewritten.size()
            << " old_bound=" << kB7ExpectedInputBound
            << " new_bound=" << kB7OutputBound << std::endl;
        return true;
    }
}

'''


def patch_translation_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "candidate-b7-mipmaps-live-range" in text:
        return
    if "#include <cstring>" not in text:
        text = replace_exact(text, "#include <cstddef>\n",
            "#include <cstddef>\n#include <cstring>\n", count=1,
            label=f"{path}: cstring include")
    if "#include <iostream>" not in text:
        text = replace_exact(text, "#include <cstdint>\n",
            "#include <cstdint>\n#include <iostream>\n", count=1,
            label=f"{path}: iostream include")
    text = replace_exact(text, "struct BindingOffsets {\n",
        helper() + "struct BindingOffsets {\n", count=1,
        label=f"{path}: Candidate B7 helper")
    b4_anchor = "    applyCandidateB4Beta4PredicateCanonicalization(shaderName, spirvBytecode);\n"
    text = replace_exact(text, b4_anchor,
        b4_anchor + "    applyCandidateB7MipmapsLiveRange(shaderName, spirvBytecode);\n",
        count=1, label=f"{path}: Candidate B7 invocation")
    path.write_text(text, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    patch_translation_source(args.root.resolve() / TRANSLATION_SOURCE)


if __name__ == "__main__":
    main()
