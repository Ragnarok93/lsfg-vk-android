#!/usr/bin/env python3
"""Apply Candidate B5 Mipmaps reduction-predicate canonicalization.

B5 redirects only the five p_mipmaps reduction branch predicates to compact
predicates derived from LocalInvocationId. The exact B4-era Mipmaps module and
exact rewritten output are fingerprinted. Sampling, barriers, shared memory,
image writes, formats and local size remain unchanged; any mismatch is a no-op.
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
    constexpr uint64_t kB5ExpectedFnv1a64 = 0x65d3c6a69e9f9b07ULL;
    constexpr size_t kB5ExpectedBytes = 28832;
    constexpr uint32_t kB5ExpectedBound = 1270;
    constexpr uint64_t kB5ExpectedOutputFnv1a64 = 0xdcd905da4b4b6edcULL;
    constexpr size_t kB5ExpectedOutputBytes = 29612;
    constexpr uint32_t kB5ExpectedOutputBound = 1310;
    constexpr size_t kB5PredicateCount = 5;

    constexpr uint16_t kB5OpLoad = static_cast<uint16_t>(spv::OpLoad);
    constexpr uint16_t kB5OpCompositeExtract = static_cast<uint16_t>(spv::OpCompositeExtract);
    constexpr uint16_t kB5OpUMod = static_cast<uint16_t>(spv::OpUMod);
    constexpr uint16_t kB5OpIEqual = static_cast<uint16_t>(spv::OpIEqual);
    constexpr uint16_t kB5OpLogicalAnd = static_cast<uint16_t>(spv::OpLogicalAnd);
    constexpr uint16_t kB5OpSelectionMerge = static_cast<uint16_t>(spv::OpSelectionMerge);
    constexpr uint16_t kB5OpBranchConditional = static_cast<uint16_t>(spv::OpBranchConditional);

    constexpr uint32_t kB5TypeV3Uint = 32;
    constexpr uint32_t kB5TypeUint = 10;
    constexpr uint32_t kB5TypeBool = 48;
    constexpr uint32_t kB5LocalInvocationId = 34;
    constexpr uint32_t kB5Zero = 40;

    constexpr uint32_t kB5OldConditions[kB5PredicateCount] = {
        670U, 790U, 931U, 1050U, 1190U,
    };
    constexpr uint32_t kB5StepConstants[kB5PredicateCount] = {
        11U, 699U, 819U, 960U, 1079U,
    };
    constexpr uint32_t kB5MergeLabels[kB5PredicateCount] = {
        672U, 792U, 933U, 1052U, 1192U,
    };
    constexpr uint32_t kB5TrueLabels[kB5PredicateCount] = {
        671U, 791U, 932U, 1051U, 1191U,
    };
    constexpr uint32_t kB5FalseLabels[kB5PredicateCount] = {
        672U, 792U, 933U, 1052U, 1192U,
    };

    uint32_t b5ReadWord(const std::vector<uint8_t>& bytecode, size_t wordIndex) {
        uint32_t word = 0;
        std::memcpy(&word, bytecode.data() + wordIndex * sizeof(uint32_t), sizeof(uint32_t));
        return word;
    }

    uint64_t b5Fnv1a64(const uint8_t* data, size_t size) {
        uint64_t hash = 1469598103934665603ULL;
        for (size_t i = 0; i < size; ++i) {
            hash ^= static_cast<uint64_t>(data[i]);
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    uint64_t b5Fnv1a64(const std::vector<uint8_t>& bytecode) {
        return b5Fnv1a64(bytecode.data(), bytecode.size());
    }

    void b5AppendOp3(std::vector<uint32_t>& words, uint16_t opCode,
            uint32_t a, uint32_t b, uint32_t c) {
        words.push_back((4U << 16U) | static_cast<uint32_t>(opCode));
        words.push_back(a); words.push_back(b); words.push_back(c);
    }

    void b5AppendOp4(std::vector<uint32_t>& words, uint16_t opCode,
            uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
        words.push_back((5U << 16U) | static_cast<uint32_t>(opCode));
        words.push_back(a); words.push_back(b); words.push_back(c); words.push_back(d);
    }

    bool applyCandidateB5MipmapsPredicateCanonicalization(
            const std::string& shaderName,
            std::vector<uint8_t>& bytecode) {
        if (shaderName != "p_mipmaps")
            return false;

        if (bytecode.size() != kB5ExpectedBytes || b5Fnv1a64(bytecode) != kB5ExpectedFnv1a64) {
            std::cerr << "lsfg-vk: candidate-b5-mipmaps-predicate-opt shader=" << shaderName
                << " applied=0 reason=fingerprint bytes=" << bytecode.size() << std::endl;
            return false;
        }

        const size_t wordCount = bytecode.size() / sizeof(uint32_t);
        if (bytecode.size() % sizeof(uint32_t) != 0 || wordCount < 5
                || b5ReadWord(bytecode, 0) != 0x07230203U
                || b5ReadWord(bytecode, 3) != kB5ExpectedBound) {
            std::cerr << "lsfg-vk: candidate-b5-mipmaps-predicate-opt shader=" << shaderName
                << " applied=0 reason=header" << std::endl;
            return false;
        }

        // Keep every original instruction. Only redirect each structured branch
        // to an equivalent LocalInvocationId divisibility predicate. The backend
        // is then free to DCE translator scratch bookkeeping proven dead.
        std::vector<uint32_t> rewritten;
        rewritten.reserve(wordCount + kB5PredicateCount * 39U);
        for (size_t i = 0; i < 5; ++i)
            rewritten.push_back(b5ReadWord(bytecode, i));

        size_t predicateIndex = 0;
        uint32_t nextId = kB5ExpectedBound;
        uint32_t pendingActiveLane = 0;
        bool pendingPredicate = false;
        size_t cursor = 5;
        while (cursor < wordCount) {
            const uint32_t firstWord = b5ReadWord(bytecode, cursor);
            const uint16_t instructionWordCount = static_cast<uint16_t>(firstWord >> 16U);
            const uint16_t opCode = static_cast<uint16_t>(firstWord & 0xffffU);
            if (instructionWordCount == 0 || cursor + instructionWordCount > wordCount) {
                std::cerr << "lsfg-vk: candidate-b5-mipmaps-predicate-opt shader=" << shaderName
                    << " applied=0 reason=malformed" << std::endl;
                return false;
            }

            if (opCode == kB5OpSelectionMerge && instructionWordCount == 3
                    && predicateIndex < kB5PredicateCount
                    && b5ReadWord(bytecode, cursor + 1) == kB5MergeLabels[predicateIndex]) {
                if (pendingPredicate) {
                    std::cerr << "lsfg-vk: candidate-b5-mipmaps-predicate-opt shader=" << shaderName
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

                b5AppendOp3(rewritten, kB5OpLoad, kB5TypeV3Uint, localId, kB5LocalInvocationId);
                b5AppendOp4(rewritten, kB5OpCompositeExtract, kB5TypeUint, x, localId, 0U);
                b5AppendOp4(rewritten, kB5OpCompositeExtract, kB5TypeUint, y, localId, 1U);
                b5AppendOp4(rewritten, kB5OpUMod, kB5TypeUint, xModulo, x,
                    kB5StepConstants[predicateIndex]);
                b5AppendOp4(rewritten, kB5OpUMod, kB5TypeUint, yModulo, y,
                    kB5StepConstants[predicateIndex]);
                b5AppendOp4(rewritten, kB5OpIEqual, kB5TypeBool, xIsZero, xModulo, kB5Zero);
                b5AppendOp4(rewritten, kB5OpIEqual, kB5TypeBool, yIsZero, yModulo, kB5Zero);
                b5AppendOp4(rewritten, kB5OpLogicalAnd, kB5TypeBool, activeLane, xIsZero, yIsZero);

                pendingActiveLane = activeLane;
                pendingPredicate = true;
                for (size_t i = 0; i < instructionWordCount; ++i)
                    rewritten.push_back(b5ReadWord(bytecode, cursor + i));
            } else if (pendingPredicate) {
                if (opCode != kB5OpBranchConditional || instructionWordCount != 4
                        || b5ReadWord(bytecode, cursor + 1) != kB5OldConditions[predicateIndex]
                        || b5ReadWord(bytecode, cursor + 2) != kB5TrueLabels[predicateIndex]
                        || b5ReadWord(bytecode, cursor + 3) != kB5FalseLabels[predicateIndex]) {
                    std::cerr << "lsfg-vk: candidate-b5-mipmaps-predicate-opt shader=" << shaderName
                        << " applied=0 reason=selection-branch-adjacency predicate="
                        << predicateIndex << std::endl;
                    return false;
                }
                rewritten.push_back(firstWord);
                rewritten.push_back(pendingActiveLane);
                rewritten.push_back(kB5TrueLabels[predicateIndex]);
                rewritten.push_back(kB5FalseLabels[predicateIndex]);
                pendingActiveLane = 0;
                pendingPredicate = false;
                ++predicateIndex;
            } else {
                for (size_t i = 0; i < instructionWordCount; ++i)
                    rewritten.push_back(b5ReadWord(bytecode, cursor + i));
            }
            cursor += instructionWordCount;
        }

        if (predicateIndex != kB5PredicateCount || pendingPredicate) {
            std::cerr << "lsfg-vk: candidate-b5-mipmaps-predicate-opt shader=" << shaderName
                << " applied=0 reason=predicate-count predicates=" << predicateIndex << std::endl;
            return false;
        }

        rewritten.at(3) = nextId;
        std::vector<uint8_t> candidate(rewritten.size() * sizeof(uint32_t));
        std::memcpy(candidate.data(), rewritten.data(), candidate.size());
        if (candidate.size() != kB5ExpectedOutputBytes
                || nextId != kB5ExpectedOutputBound
                || b5Fnv1a64(candidate) != kB5ExpectedOutputFnv1a64) {
            std::cerr << "lsfg-vk: candidate-b5-mipmaps-predicate-opt shader=" << shaderName
                << " applied=0 reason=post-fingerprint words=" << rewritten.size()
                << " bound=" << nextId << std::endl;
            return false;
        }

        bytecode = std::move(candidate);
        std::cerr << "lsfg-vk: candidate-b5-mipmaps-predicate-opt shader=" << shaderName
            << " applied=1 predicates=" << predicateIndex
            << " old_words=" << wordCount << " new_words=" << rewritten.size()
            << " old_bound=" << kB5ExpectedBound << " new_bound=" << nextId << std::endl;
        return true;
    }
}

'''


def patch_translation_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "candidate-b5-mipmaps-predicate-opt" in text:
        return
    if "#include <cstring>" not in text:
        text = replace_exact(text, "#include <cstddef>\n", "#include <cstddef>\n#include <cstring>\n",
            count=1, label=f"{path}: cstring include")
    text = replace_exact(text, "struct BindingOffsets {\n", helper() + "struct BindingOffsets {\n",
        count=1, label=f"{path}: Candidate B5 helper")

    b4_anchor = "    applyCandidateB4Beta4PredicateCanonicalization(shaderName, spirvBytecode);\n"
    if b4_anchor not in text:
        raise RuntimeError(f"{path}: Candidate B5 requires Candidate B4 composition")
    text = replace_exact(text, b4_anchor,
        b4_anchor + "    applyCandidateB5MipmapsPredicateCanonicalization(shaderName, spirvBytecode);\n",
        count=1, label=f"{path}: Candidate B5 invocation")
    path.write_text(text, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    patch_translation_source(args.root.resolve() / TRANSLATION_SOURCE)

if __name__ == "__main__":
    main()
