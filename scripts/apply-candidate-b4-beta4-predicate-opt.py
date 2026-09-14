#!/usr/bin/env python3
"""Apply Candidate B4's exact Beta-4 reduction-predicate canonicalization.

The B3 device capture proved that p_beta[4] contains five identical 81-instruction
DXBC register-emulation blocks between its workgroup barriers and reduction
branches. Those blocks only derive the 2/4/8/16/32 lane predicates and final r8
scratch masks. B4 replaces each exact block with a compact equivalent while
leaving all texture samples, barriers, shared-memory accesses, image writes,
workgroup size, formats, and interpolation math untouched.

Safety is deliberately strict: only the exact B3 p_beta[4] SPIR-V fingerprint is
accepted, and the rewritten module must match a second exact output fingerprint
before it replaces the original bytecode. Any translator drift becomes a safe
no-op with a diagnostic marker.
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


def beta4_optimizer_helper() -> str:
    return r'''namespace {
    constexpr uint64_t kB4ExpectedInputFnv = 0x975df8da92d9c418ULL;
    constexpr uint64_t kB4ExpectedOutputFnv = 0xcc6ef6a9a87ef769ULL;
    constexpr size_t kB4ExpectedInputBytes = 49984;
    constexpr size_t kB4ExpectedOutputBytes = 44384;
    constexpr size_t kB4ExpectedStages = 5;
    constexpr size_t kB4ExpectedRemovedInstructionsPerStage = 81;
    constexpr uint32_t kB4ExpectedInputBound = 2311;
    constexpr uint32_t kB4ExpectedOutputBound = 2386;
    constexpr uint32_t kB4LocalInvocationVectorId = 40;
    constexpr uint32_t kB4UintTypeId = 10;
    constexpr uint32_t kB4FloatTypeId = 8;
    constexpr uint32_t kB4Float4TypeId = 9;
    constexpr uint32_t kB4BoolTypeId = 61;
    constexpr uint32_t kB4ZeroUintId = 54;
    constexpr uint32_t kB4ZeroFloatId = 67;
    constexpr uint32_t kB4AllBitsUintId = 950;
    constexpr uint32_t kB4ScratchRegisterId = 370;

    struct B4Instruction {
        size_t offset{};
        uint16_t wordCount{};
        uint16_t opCode{};
    };

    struct B4StagePatch {
        size_t beginWord{};
        size_t endWord{};
        size_t selectionWord{};
        size_t branchWord{};
        uint32_t divisorId{};
        uint32_t firstResultId{};
        uint32_t conditionId{};
    };

    uint32_t b4ReadWord(const std::vector<uint8_t>& bytecode, size_t wordIndex) {
        const size_t offset = wordIndex * sizeof(uint32_t);
        return static_cast<uint32_t>(bytecode.at(offset))
            | (static_cast<uint32_t>(bytecode.at(offset + 1)) << 8U)
            | (static_cast<uint32_t>(bytecode.at(offset + 2)) << 16U)
            | (static_cast<uint32_t>(bytecode.at(offset + 3)) << 24U);
    }

    uint64_t b4Fnv1a(const std::vector<uint8_t>& bytecode) {
        uint64_t hash = 0xcbf29ce484222325ULL;
        for (const uint8_t value : bytecode) {
            hash ^= value;
            hash *= 0x100000001b3ULL;
        }
        return hash;
    }

    void b4AppendInstruction(
            std::vector<uint32_t>& words,
            uint16_t opCode,
            std::initializer_list<uint32_t> operands) {
        const uint32_t wordCount = static_cast<uint32_t>(operands.size() + 1);
        words.push_back((wordCount << 16U) | static_cast<uint32_t>(opCode));
        words.insert(words.end(), operands.begin(), operands.end());
    }

    void b4AppendStageReplacement(
            std::vector<uint32_t>& words,
            const B4StagePatch& patch) {
        uint32_t id = patch.firstResultId;
        const uint32_t x = id++;
        const uint32_t y = id++;
        const uint32_t xRemainder = id++;
        const uint32_t yRemainder = id++;
        const uint32_t xZero = id++;
        const uint32_t yZero = id++;
        const uint32_t condition = id++;
        const uint32_t xMask = id++;
        const uint32_t yMask = id++;
        const uint32_t xMaskFloat = id++;
        const uint32_t yMaskFloat = id++;
        const uint32_t oldScratch = id++;
        const uint32_t scratchX = id++;
        const uint32_t scratchXY = id++;
        const uint32_t scratchXYZ = id++;

        b4AppendInstruction(words, spv::OpCompositeExtract,
            {kB4UintTypeId, x, kB4LocalInvocationVectorId, 0});
        b4AppendInstruction(words, spv::OpCompositeExtract,
            {kB4UintTypeId, y, kB4LocalInvocationVectorId, 1});
        b4AppendInstruction(words, spv::OpUMod,
            {kB4UintTypeId, xRemainder, x, patch.divisorId});
        b4AppendInstruction(words, spv::OpUMod,
            {kB4UintTypeId, yRemainder, y, patch.divisorId});
        b4AppendInstruction(words, spv::OpIEqual,
            {kB4BoolTypeId, xZero, xRemainder, kB4ZeroUintId});
        b4AppendInstruction(words, spv::OpIEqual,
            {kB4BoolTypeId, yZero, yRemainder, kB4ZeroUintId});
        b4AppendInstruction(words, spv::OpLogicalAnd,
            {kB4BoolTypeId, condition, xZero, yZero});
        b4AppendInstruction(words, spv::OpSelect,
            {kB4UintTypeId, xMask, condition, kB4AllBitsUintId, kB4ZeroUintId});
        b4AppendInstruction(words, spv::OpSelect,
            {kB4UintTypeId, yMask, yZero, kB4AllBitsUintId, kB4ZeroUintId});
        b4AppendInstruction(words, spv::OpBitcast,
            {kB4FloatTypeId, xMaskFloat, xMask});
        b4AppendInstruction(words, spv::OpBitcast,
            {kB4FloatTypeId, yMaskFloat, yMask});
        b4AppendInstruction(words, spv::OpLoad,
            {kB4Float4TypeId, oldScratch, kB4ScratchRegisterId});
        b4AppendInstruction(words, spv::OpCompositeInsert,
            {kB4Float4TypeId, scratchX, xMaskFloat, oldScratch, 0});
        b4AppendInstruction(words, spv::OpCompositeInsert,
            {kB4Float4TypeId, scratchXY, yMaskFloat, scratchX, 1});
        b4AppendInstruction(words, spv::OpCompositeInsert,
            {kB4Float4TypeId, scratchXYZ, kB4ZeroFloatId, scratchXY, 2});
        b4AppendInstruction(words, spv::OpStore,
            {kB4ScratchRegisterId, scratchXYZ});
    }

    bool optimizeBeta4ReductionPredicates(
            const std::string& shaderName,
            std::vector<uint8_t>& bytecode) {
        if (shaderName != "p_beta[4]")
            return false;

        const uint64_t inputHash = b4Fnv1a(bytecode);
        if (bytecode.size() != kB4ExpectedInputBytes
                || inputHash != kB4ExpectedInputFnv
                || bytecode.size() % sizeof(uint32_t) != 0) {
            std::cerr << "lsfg-vk: beta4-predicate-opt shader=" << shaderName
                << " applied=0 reason=input-fingerprint bytes=" << bytecode.size()
                << " fnv=0x" << std::hex << inputHash << std::dec << std::endl;
            return false;
        }

        const size_t wordCount = bytecode.size() / sizeof(uint32_t);
        std::vector<uint32_t> originalWords;
        originalWords.reserve(wordCount);
        for (size_t word = 0; word < wordCount; ++word)
            originalWords.push_back(b4ReadWord(bytecode, word));

        if (originalWords.size() < 5
                || originalWords.at(0) != 0x07230203U
                || originalWords.at(3) != kB4ExpectedInputBound)
            return false;

        std::vector<B4Instruction> instructions;
        size_t cursor = 5;
        while (cursor < originalWords.size()) {
            const uint32_t firstWord = originalWords.at(cursor);
            const uint16_t instructionWords = static_cast<uint16_t>(firstWord >> 16U);
            const uint16_t opCode = static_cast<uint16_t>(firstWord & 0xffffU);
            if (instructionWords == 0 || cursor + instructionWords > originalWords.size())
                return false;
            instructions.push_back({cursor, instructionWords, opCode});
            cursor += instructionWords;
        }

        const uint32_t divisors[kB4ExpectedStages] = {1007, 986, 1403, 1667, 1930};
        std::vector<B4StagePatch> patches;
        uint32_t nextResultId = kB4ExpectedInputBound;
        for (size_t instruction = 0; instruction < instructions.size(); ++instruction) {
            if (instructions.at(instruction).opCode != spv::OpControlBarrier)
                continue;
            if (patches.size() >= kB4ExpectedStages)
                return false;

            size_t selection = instruction + 1;
            while (selection < instructions.size()
                    && instructions.at(selection).opCode != spv::OpSelectionMerge) {
                const uint16_t opCode = instructions.at(selection).opCode;
                if (opCode == spv::OpLabel
                        || opCode == spv::OpBranch
                        || opCode == spv::OpBranchConditional
                        || opCode == spv::OpControlBarrier)
                    return false;
                ++selection;
            }
            if (selection >= instructions.size()
                    || selection + 1 >= instructions.size()
                    || instructions.at(selection + 1).opCode != spv::OpBranchConditional)
                return false;

            const size_t removedInstructions = selection - (instruction + 1);
            if (removedInstructions != kB4ExpectedRemovedInstructionsPerStage)
                return false;

            size_t scratchStores = 0;
            for (size_t candidate = instruction + 1; candidate < selection; ++candidate) {
                if (instructions.at(candidate).opCode != spv::OpStore)
                    continue;
                const size_t storeWord = instructions.at(candidate).offset;
                if (instructions.at(candidate).wordCount < 3
                        || originalWords.at(storeWord + 1) != kB4ScratchRegisterId)
                    return false;
                ++scratchStores;
            }
            if (scratchStores != 9)
                return false;

            B4StagePatch patch{};
            patch.beginWord = instructions.at(instruction + 1).offset;
            patch.endWord = instructions.at(selection).offset;
            patch.selectionWord = instructions.at(selection).offset;
            patch.branchWord = instructions.at(selection + 1).offset;
            patch.divisorId = divisors[patches.size()];
            patch.firstResultId = nextResultId;
            patch.conditionId = nextResultId + 6;
            nextResultId += 15;
            patches.push_back(patch);
        }

        if (patches.size() != kB4ExpectedStages
                || nextResultId != kB4ExpectedOutputBound)
            return false;

        std::vector<uint32_t> optimizedWords;
        optimizedWords.reserve(kB4ExpectedOutputBytes / sizeof(uint32_t));
        optimizedWords.insert(
            optimizedWords.end(), originalWords.begin(), originalWords.begin() + 5);

        cursor = 5;
        size_t stage = 0;
        while (cursor < originalWords.size()) {
            if (stage < patches.size() && cursor == patches.at(stage).beginWord) {
                b4AppendStageReplacement(optimizedWords, patches.at(stage));
                cursor = patches.at(stage).endWord;
                continue;
            }

            const uint32_t firstWord = originalWords.at(cursor);
            const uint16_t instructionWords = static_cast<uint16_t>(firstWord >> 16U);
            if (instructionWords == 0 || cursor + instructionWords > originalWords.size())
                return false;

            if (stage < patches.size() && cursor == patches.at(stage).branchWord) {
                if ((firstWord & 0xffffU) != spv::OpBranchConditional
                        || instructionWords < 4)
                    return false;
                optimizedWords.push_back(firstWord);
                optimizedWords.push_back(patches.at(stage).conditionId);
                for (uint16_t operand = 2; operand < instructionWords; ++operand)
                    optimizedWords.push_back(originalWords.at(cursor + operand));
                ++stage;
            } else {
                optimizedWords.insert(
                    optimizedWords.end(),
                    originalWords.begin() + static_cast<std::ptrdiff_t>(cursor),
                    originalWords.begin() + static_cast<std::ptrdiff_t>(cursor + instructionWords));
            }
            cursor += instructionWords;
        }

        if (stage != kB4ExpectedStages || optimizedWords.size() < 5)
            return false;
        optimizedWords.at(3) = kB4ExpectedOutputBound;

        std::vector<uint8_t> optimizedBytecode;
        optimizedBytecode.reserve(optimizedWords.size() * sizeof(uint32_t));
        for (const uint32_t word : optimizedWords) {
            optimizedBytecode.push_back(static_cast<uint8_t>(word & 0xffU));
            optimizedBytecode.push_back(static_cast<uint8_t>((word >> 8U) & 0xffU));
            optimizedBytecode.push_back(static_cast<uint8_t>((word >> 16U) & 0xffU));
            optimizedBytecode.push_back(static_cast<uint8_t>((word >> 24U) & 0xffU));
        }

        const uint64_t outputHash = b4Fnv1a(optimizedBytecode);
        if (optimizedBytecode.size() != kB4ExpectedOutputBytes
                || outputHash != kB4ExpectedOutputFnv) {
            std::cerr << "lsfg-vk: beta4-predicate-opt shader=" << shaderName
                << " applied=0 reason=output-fingerprint bytes=" << optimizedBytecode.size()
                << " fnv=0x" << std::hex << outputHash << std::dec << std::endl;
            return false;
        }

        bytecode.swap(optimizedBytecode);
        std::cerr << "lsfg-vk: beta4-predicate-opt shader=" << shaderName
            << " applied=1 stages=" << kB4ExpectedStages
            << " input_bytes=" << kB4ExpectedInputBytes
            << " output_bytes=" << bytecode.size()
            << " input_instructions=2769 output_instructions=2444"
            << std::endl;
        return true;
    }
}

'''


def patch_translation_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "kB4ExpectedInputFnv" in text:
        return

    if "#include <initializer_list>" not in text:
        anchor = "#include <algorithm>\n"
        if anchor not in text:
            raise RuntimeError(f"{path}: algorithm include anchor missing")
        text = text.replace(anchor, anchor + "#include <initializer_list>\n", 1)

    signature = "std::vector<uint8_t> Extract::translateShader(\n"
    if signature not in text:
        raise RuntimeError(f"{path}: named translator signature missing; B4 must run after Candidate B cleanup")
    text = text.replace(signature, beta4_optimizer_helper() + signature, 1)

    profiling_anchor = "    logBeta4DependencyProfile(shaderName, spirvBytecode);\n"
    call = "    optimizeBeta4ReductionPredicates(shaderName, spirvBytecode);\n"
    if profiling_anchor in text:
        text = text.replace(profiling_anchor, profiling_anchor + call, 1)
    else:
        return_anchor = "    return spirvBytecode;\n"
        found = text.count(return_anchor)
        if found != 1:
            raise RuntimeError(f"{path}: expected one shader return, found {found}")
        text = text.replace(return_anchor, call + return_anchor, 1)

    path.write_text(text, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    patch_translation_source(args.root.resolve() / TRANSLATION_SOURCE)


if __name__ == "__main__":
    main()
