#!/usr/bin/env python3
"""Apply the lossless Candidate B4 Beta-4 reduction-predicate canonicalization.

B4 redirects only the five Performance p_beta[4] reduction branch predicates to
compact predicates derived from LocalInvocationId. The exact B3 module is
fingerprinted and every structured-control-flow ID is checked before mutation.
Sampling, barriers, shared memory, image writes, formats and local size are left
unchanged; any mismatch returns the original module.
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
    constexpr uint64_t kB4ExpectedFnv1a64 = 0x7887fee01585449eULL;
    constexpr size_t kB4ExpectedBytes = 49984;
    constexpr uint32_t kB4ExpectedBound = 2311;
    constexpr size_t kB4PredicateCount = 5;

    constexpr uint16_t kB4OpLoad = static_cast<uint16_t>(spv::OpLoad);
    constexpr uint16_t kB4OpCompositeExtract = static_cast<uint16_t>(spv::OpCompositeExtract);
    constexpr uint16_t kB4OpUMod = static_cast<uint16_t>(spv::OpUMod);
    constexpr uint16_t kB4OpIEqual = static_cast<uint16_t>(spv::OpIEqual);
    constexpr uint16_t kB4OpLogicalAnd = static_cast<uint16_t>(spv::OpLogicalAnd);
    constexpr uint16_t kB4OpSelectionMerge = static_cast<uint16_t>(spv::OpSelectionMerge);
    constexpr uint16_t kB4OpBranchConditional = static_cast<uint16_t>(spv::OpBranchConditional);

    constexpr uint32_t kB4TypeV3Uint = 32;
    constexpr uint32_t kB4TypeUint = 10;
    constexpr uint32_t kB4TypeBool = 61;
    constexpr uint32_t kB4LocalInvocationId = 34;
    constexpr uint32_t kB4Zero = 54;

    constexpr uint32_t kB4OldConditions[kB4PredicateCount] = {
        1080U, 1353U, 1617U, 1880U, 2143U,
    };
    constexpr uint32_t kB4StepConstants[kB4PredicateCount] = {
        1007U, 986U, 1403U, 1667U, 1930U,
    };
    constexpr uint32_t kB4MergeLabels[kB4PredicateCount] = {
        1082U, 1355U, 1619U, 1882U, 2145U,
    };
    constexpr uint32_t kB4TrueLabels[kB4PredicateCount] = {
        1081U, 1354U, 1618U, 1881U, 2144U,
    };
    constexpr uint32_t kB4FalseLabels[kB4PredicateCount] = {
        1277U, 1355U, 1619U, 1882U, 2145U,
    };

    uint32_t b4ReadWord(const std::vector<uint8_t>& bytecode, size_t wordIndex) {
        uint32_t word = 0;
        std::memcpy(&word, bytecode.data() + wordIndex * sizeof(uint32_t), sizeof(uint32_t));
        return word;
    }

    uint64_t b4Fnv1a64(const std::vector<uint8_t>& bytecode) {
        uint64_t hash = 1469598103934665603ULL;
        for (const uint8_t value : bytecode) {
            hash ^= static_cast<uint64_t>(value);
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    void b4AppendOp3(std::vector<uint32_t>& words, uint16_t opCode,
            uint32_t a, uint32_t b, uint32_t c) {
        words.push_back((4U << 16U) | static_cast<uint32_t>(opCode));
        words.push_back(a); words.push_back(b); words.push_back(c);
    }

    void b4AppendOp4(std::vector<uint32_t>& words, uint16_t opCode,
            uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
        words.push_back((5U << 16U) | static_cast<uint32_t>(opCode));
        words.push_back(a); words.push_back(b); words.push_back(c); words.push_back(d);
    }

    bool applyCandidateB4Beta4PredicateCanonicalization(
            const std::string& shaderName,
            std::vector<uint8_t>& bytecode) {
        if (shaderName != "p_beta[4]")
            return false;

        if (bytecode.size() != kB4ExpectedBytes || b4Fnv1a64(bytecode) != kB4ExpectedFnv1a64) {
            std::cerr << "lsfg-vk: candidate-b4-beta4-predicate-opt shader=" << shaderName
                << " applied=0 reason=fingerprint bytes=" << bytecode.size() << std::endl;
            return false;
        }

        const size_t wordCount = bytecode.size() / sizeof(uint32_t);
        if (bytecode.size() % sizeof(uint32_t) != 0 || wordCount < 5
                || b4ReadWord(bytecode, 0) != 0x07230203U
                || b4ReadWord(bytecode, 3) != kB4ExpectedBound) {
            std::cerr << "lsfg-vk: candidate-b4-beta4-predicate-opt shader=" << shaderName
                << " applied=0 reason=header" << std::endl;
            return false;
        }

        // Emit each direct predicate BEFORE OpSelectionMerge. SPIR-V requires
        // SelectionMerge to remain immediately adjacent to BranchConditional.
        std::vector<uint32_t> rewritten;
        rewritten.reserve(wordCount + kB4PredicateCount * 39U);
        for (size_t i = 0; i < 5; ++i)
            rewritten.push_back(b4ReadWord(bytecode, i));

        size_t predicateIndex = 0;
        uint32_t nextId = kB4ExpectedBound;
        uint32_t pendingActiveLane = 0;
        bool pendingPredicate = false;
        size_t cursor = 5;
        while (cursor < wordCount) {
            const uint32_t firstWord = b4ReadWord(bytecode, cursor);
            const uint16_t instructionWordCount = static_cast<uint16_t>(firstWord >> 16U);
            const uint16_t opCode = static_cast<uint16_t>(firstWord & 0xffffU);
            if (instructionWordCount == 0 || cursor + instructionWordCount > wordCount) {
                std::cerr << "lsfg-vk: candidate-b4-beta4-predicate-opt shader=" << shaderName
                    << " applied=0 reason=malformed" << std::endl;
                return false;
            }

            if (opCode == kB4OpSelectionMerge && instructionWordCount == 3
                    && predicateIndex < kB4PredicateCount
                    && b4ReadWord(bytecode, cursor + 1) == kB4MergeLabels[predicateIndex]) {
                if (pendingPredicate) {
                    std::cerr << "lsfg-vk: candidate-b4-beta4-predicate-opt shader=" << shaderName
                        << " applied=0 reason=nested-predicate" << std::endl;
                    return false;
                }

                const uint32_t localId = nextId++;
                const uint32_t x = nextId++;
                const uint32_t y = nextId++;
                const uint32_t xModulo = nextId++;
                const uint32_t yModulo = nextId++;
                const uint32_t xIsZero = nextId++;
                const uint32_t yIsZero = nextId++;
                const uint32_t activeLane = nextId++;

                b4AppendOp3(rewritten, kB4OpLoad, kB4TypeV3Uint, localId, kB4LocalInvocationId);
                b4AppendOp4(rewritten, kB4OpCompositeExtract, kB4TypeUint, x, localId, 0U);
                b4AppendOp4(rewritten, kB4OpCompositeExtract, kB4TypeUint, y, localId, 1U);
                b4AppendOp4(rewritten, kB4OpUMod, kB4TypeUint, xModulo, x,
                    kB4StepConstants[predicateIndex]);
                b4AppendOp4(rewritten, kB4OpUMod, kB4TypeUint, yModulo, y,
                    kB4StepConstants[predicateIndex]);
                b4AppendOp4(rewritten, kB4OpIEqual, kB4TypeBool, xIsZero, xModulo, kB4Zero);
                b4AppendOp4(rewritten, kB4OpIEqual, kB4TypeBool, yIsZero, yModulo, kB4Zero);
                b4AppendOp4(rewritten, kB4OpLogicalAnd, kB4TypeBool, activeLane, xIsZero, yIsZero);

                pendingActiveLane = activeLane;
                pendingPredicate = true;
                for (size_t i = 0; i < instructionWordCount; ++i)
                    rewritten.push_back(b4ReadWord(bytecode, cursor + i));
            } else if (pendingPredicate) {
                if (opCode != kB4OpBranchConditional || instructionWordCount != 4
                        || b4ReadWord(bytecode, cursor + 1) != kB4OldConditions[predicateIndex]
                        || b4ReadWord(bytecode, cursor + 2) != kB4TrueLabels[predicateIndex]
                        || b4ReadWord(bytecode, cursor + 3) != kB4FalseLabels[predicateIndex]) {
                    std::cerr << "lsfg-vk: candidate-b4-beta4-predicate-opt shader=" << shaderName
                        << " applied=0 reason=selection-branch-adjacency predicate="
                        << predicateIndex << std::endl;
                    return false;
                }
                rewritten.push_back(firstWord);
                rewritten.push_back(pendingActiveLane);
                rewritten.push_back(kB4TrueLabels[predicateIndex]);
                rewritten.push_back(kB4FalseLabels[predicateIndex]);
                pendingActiveLane = 0;
                pendingPredicate = false;
                ++predicateIndex;
            } else {
                for (size_t i = 0; i < instructionWordCount; ++i)
                    rewritten.push_back(b4ReadWord(bytecode, cursor + i));
            }
            cursor += instructionWordCount;
        }

        if (predicateIndex != kB4PredicateCount || pendingPredicate) {
            std::cerr << "lsfg-vk: candidate-b4-beta4-predicate-opt shader=" << shaderName
                << " applied=0 reason=predicate-count predicates=" << predicateIndex << std::endl;
            return false;
        }

        rewritten.at(3) = nextId;
        bytecode.resize(rewritten.size() * sizeof(uint32_t));
        std::memcpy(bytecode.data(), rewritten.data(), bytecode.size());
        std::cerr << "lsfg-vk: candidate-b4-beta4-predicate-opt shader=" << shaderName
            << " applied=1 predicates=" << predicateIndex
            << " old_words=" << wordCount << " new_words=" << rewritten.size()
            << " old_bound=" << kB4ExpectedBound << " new_bound=" << nextId << std::endl;
        return true;
    }
}

'''


def patch_translation_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "candidate-b4-beta4-predicate-opt" in text:
        return
    if "#include <cstring>" not in text:
        text = replace_exact(text, "#include <cstddef>\n", "#include <cstddef>\n#include <cstring>\n",
            count=1, label=f"{path}: cstring include")
    text = replace_exact(text, "struct BindingOffsets {\n", helper() + "struct BindingOffsets {\n",
        count=1, label=f"{path}: Candidate B4 helper")

    profiled_anchor = "    logMipmapsSpirvProfile(shaderName, spirvBytecode);\n"
    if profiled_anchor in text:
        text = replace_exact(text, profiled_anchor,
            "    applyCandidateB4Beta4PredicateCanonicalization(shaderName, spirvBytecode);\n" + profiled_anchor,
            count=1, label=f"{path}: profiled Candidate B4 invocation")
    else:
        if "const std::string& shaderName" not in text:
            raise RuntimeError(f"{path}: Candidate B4 requires named translation from Candidate B cleanup")
        text = replace_exact(text, "    return spirvBytecode;\n",
            "    applyCandidateB4Beta4PredicateCanonicalization(shaderName, spirvBytecode);\n"
            "    return spirvBytecode;\n",
            count=1, label=f"{path}: production Candidate B4 invocation")
    path.write_text(text, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    patch_translation_source(args.root.resolve() / TRANSLATION_SOURCE)


if __name__ == "__main__":
    main()
