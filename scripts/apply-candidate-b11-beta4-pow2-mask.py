#!/usr/bin/env python3
"""Strength-reduce retained B4 Beta4 power-of-two reduction predicates.

Candidate B11 is deliberately narrower than B7/B9/B10. It runs after B4 and
only changes B4's ten generated unsigned modulo operations (x/y for five
reduction predicates) to bitwise masks when the exact B4 input SPIR-V proves
all five divisors are powers of two and already contains each divisor-minus-one
mask constant. Any mismatch leaves ordinary retained B4 OpUMod emission intact.

No image operation, barrier, shared-memory access, local size, format, floating
expression, sampling order, synchronization, scheduler, or presentation logic
is modified.
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
    return r'''    bool b11ResolvePow2Masks(const std::vector<uint8_t>& bytecode,
            uint32_t* maskConstants, uint32_t* stepValues, const char*& reason) {
        std::vector<uint8_t> constantSeen(kB4ExpectedBound, 0U);
        std::vector<uint32_t> constantValues(kB4ExpectedBound, 0U);
        const size_t wordCount = bytecode.size() / sizeof(uint32_t);

        for (size_t cursor = 5; cursor < wordCount;) {
            const uint32_t firstWord = b4ReadWord(bytecode, cursor);
            const uint16_t instructionWordCount = static_cast<uint16_t>(firstWord >> 16U);
            const uint16_t opCode = static_cast<uint16_t>(firstWord & 0xffffU);
            if (instructionWordCount == 0 || cursor + instructionWordCount > wordCount) {
                reason = "malformed-constants";
                return false;
            }
            if (opCode == kB11OpConstant && instructionWordCount == 4
                    && b4ReadWord(bytecode, cursor + 1) == kB4TypeUint) {
                const uint32_t resultId = b4ReadWord(bytecode, cursor + 2);
                if (resultId >= kB4ExpectedBound || constantSeen[resultId] != 0U) {
                    reason = "constant-id-invalid";
                    return false;
                }
                constantSeen[resultId] = 1U;
                constantValues[resultId] = b4ReadWord(bytecode, cursor + 3);
            }
            cursor += instructionWordCount;
        }

        for (size_t predicate = 0; predicate < kB4PredicateCount; ++predicate) {
            const uint32_t stepId = kB4StepConstants[predicate];
            if (stepId >= kB4ExpectedBound || constantSeen[stepId] == 0U) {
                reason = "step-constant-missing";
                return false;
            }
            const uint32_t value = constantValues[stepId];
            if (!(value != 0U && (value & (value - 1U)) == 0U)) {
                reason = "step-not-power-of-two";
                return false;
            }

            const uint32_t maskValue = value - 1U;
            uint32_t maskId = 0U;
            bool foundMask = false;
            for (uint32_t id = 0; id < kB4ExpectedBound; ++id) {
                if (constantSeen[id] != 0U && constantValues[id] == maskValue) {
                    maskId = id;
                    foundMask = true;
                    break;
                }
            }
            if (!foundMask) {
                reason = "mask-constant-missing";
                return false;
            }
            stepValues[predicate] = value;
            maskConstants[predicate] = maskId;
        }

        reason = "ok";
        return true;
    }

'''


def patch_translation_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "candidate-b11-beta4-pow2-mask" in text:
        return
    if "candidate-b4-beta4-predicate-opt" not in text:
        raise RuntimeError(f"{path}: Candidate B11 requires retained Candidate B4")

    text = replace_exact(
        text,
        "    constexpr uint16_t kB4OpUMod = static_cast<uint16_t>(spv::OpUMod);\n",
        "    constexpr uint16_t kB4OpUMod = static_cast<uint16_t>(spv::OpUMod);\n"
        "    constexpr uint16_t kB11OpConstant = static_cast<uint16_t>(spv::OpConstant);\n"
        "    constexpr uint16_t kB11OpBitwiseAnd = static_cast<uint16_t>(spv::OpBitwiseAnd);\n",
        count=1,
        label=f"{path}: B11 opcodes",
    )

    text = replace_exact(
        text,
        "    bool applyCandidateB4Beta4PredicateCanonicalization(\n",
        helper() + "    bool applyCandidateB4Beta4PredicateCanonicalization(\n",
        count=1,
        label=f"{path}: B11 constant resolver",
    )

    b11_setup = '''        uint32_t b11MaskConstants[kB4PredicateCount] = {};\n        uint32_t b11StepValues[kB4PredicateCount] = {};\n        const char* b11Reason = "not-checked";\n        const bool b11Enabled = b11ResolvePow2Masks(\n            bytecode, b11MaskConstants, b11StepValues, b11Reason);\n\n'''
    text = replace_exact(
        text,
        "        // Emit each direct predicate BEFORE OpSelectionMerge. SPIR-V requires\n",
        b11_setup + "        // Emit each direct predicate BEFORE OpSelectionMerge. SPIR-V requires\n",
        count=1,
        label=f"{path}: B11 setup",
    )

    old_modulo = '''                b4AppendOp4(rewritten, kB4OpUMod, kB4TypeUint, xModulo, x,\n                    kB4StepConstants[predicateIndex]);\n                b4AppendOp4(rewritten, kB4OpUMod, kB4TypeUint, yModulo, y,\n                    kB4StepConstants[predicateIndex]);\n'''
    new_mask = '''                const uint16_t b11PredicateOp =\n                    b11Enabled ? kB11OpBitwiseAnd : kB4OpUMod;\n                const uint32_t b11PredicateRhs = b11Enabled\n                    ? b11MaskConstants[predicateIndex]\n                    : kB4StepConstants[predicateIndex];\n                b4AppendOp4(rewritten, b11PredicateOp, kB4TypeUint, xModulo, x,\n                    b11PredicateRhs);\n                b4AppendOp4(rewritten, b11PredicateOp, kB4TypeUint, yModulo, y,\n                    b11PredicateRhs);\n'''
    text = replace_exact(
        text,
        old_modulo,
        new_mask,
        count=1,
        label=f"{path}: B11 predicate strength reduction",
    )

    b4_log = '''        std::cerr << "lsfg-vk: candidate-b4-beta4-predicate-opt shader=" << shaderName\n'''
    b11_log = '''        if (b11Enabled) {\n            std::cerr << "lsfg-vk: candidate-b11-beta4-pow2-mask shader=" << shaderName\n                << " applied=1 predicates=" << kB4PredicateCount << " divisors=";\n            for (size_t i = 0; i < kB4PredicateCount; ++i) {\n                if (i != 0U)\n                    std::cerr << ',';\n                std::cerr << b11StepValues[i];\n            }\n            std::cerr << std::endl;\n        } else {\n            std::cerr << "lsfg-vk: candidate-b11-beta4-pow2-mask shader=" << shaderName\n                << " applied=0 reason=" << b11Reason << std::endl;\n        }\n\n'''
    text = replace_exact(
        text,
        b4_log,
        b11_log + b4_log,
        count=1,
        label=f"{path}: B11 telemetry",
    )

    path.write_text(text, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    patch_translation_source(args.root.resolve() / TRANSLATION_SOURCE)


if __name__ == "__main__":
    main()
