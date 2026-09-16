#!/usr/bin/env python3
"""Fuse and hoist the retained Beta4 power-of-two lane predicates.

B13 runs after B4 and B11.  When B11 has proved that every reduction step is
a power of two, B13 emits the exact identity

    ((x | y) & (step - 1)) == 0

instead of testing the masked x and y coordinates separately.  The local
invocation ID and its x/y components are emitted once before the first
structured selection and dominate all later reduction selections.  If B11's
runtime proof fails, the original B4 modulo predicate path is emitted without
any B13 mutation.
"""
from __future__ import annotations

import argparse
from pathlib import Path


TRANSLATION_SOURCE = Path("src/extract/trans.cpp")
TARGET_SHADER = "p_beta[4]"
MARKER = "candidate-b13-beta4-fused-mask"


def replace_exact(text: str, old: str, new: str, *, count: int, label: str) -> str:
    if new in text:
        return text
    found = text.count(old)
    if found != count:
        raise RuntimeError(f"{label}: expected {count} match(es), found {found}")
    return text.replace(old, new, count)


def patch_translation_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if MARKER in text:
        return
    if "candidate-b11-beta4-pow2-mask" not in text:
        raise RuntimeError(f"{path}: Candidate B13 requires retained B11 for {TARGET_SHADER}")

    text = replace_exact(
        text,
        "    constexpr uint16_t kB11OpBitwiseAnd = static_cast<uint16_t>(spv::OpBitwiseAnd);\n",
        "    constexpr uint16_t kB11OpBitwiseAnd = static_cast<uint16_t>(spv::OpBitwiseAnd);\n"
        "    constexpr uint16_t kB13OpBitwiseOr = static_cast<uint16_t>(spv::OpBitwiseOr);\n",
        count=1,
        label=f"{path}: B13 opcode",
    )

    text = replace_exact(
        text,
        "        uint32_t pendingActiveLane = 0;\n"
        "        bool pendingPredicate = false;\n",
        "        uint32_t pendingActiveLane = 0;\n"
        "        uint32_t b13SharedLocalId = 0U;\n"
        "        uint32_t b13SharedX = 0U;\n"
        "        uint32_t b13SharedY = 0U;\n"
        "        uint32_t b13SharedXY = 0U;\n"
        "        bool b13CoordinatesEmitted = false;\n"
        "        bool pendingPredicate = false;\n",
        count=1,
        label=f"{path}: B13 shared coordinates",
    )

    old_predicate = '''                const uint32_t localId = nextId++;
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
                const uint16_t b11PredicateOp =
                    b11Enabled ? kB11OpBitwiseAnd : kB4OpUMod;
                const uint32_t b11PredicateRhs = b11Enabled
                    ? b11MaskConstants[predicateIndex]
                    : kB4StepConstants[predicateIndex];
                b4AppendOp4(rewritten, b11PredicateOp, kB4TypeUint, xModulo, x,
                    b11PredicateRhs);
                b4AppendOp4(rewritten, b11PredicateOp, kB4TypeUint, yModulo, y,
                    b11PredicateRhs);
                b4AppendOp4(rewritten, kB4OpIEqual, kB4TypeBool, xIsZero, xModulo, kB4Zero);
                b4AppendOp4(rewritten, kB4OpIEqual, kB4TypeBool, yIsZero, yModulo, kB4Zero);
                b4AppendOp4(rewritten, kB4OpLogicalAnd, kB4TypeBool, activeLane, xIsZero, yIsZero);
'''
    new_predicate = '''                uint32_t activeLane = 0U;
                if (b11Enabled) {
                    if (!b13CoordinatesEmitted) {
                        b13SharedLocalId = nextId++;
                        b13SharedX = nextId++;
                        b13SharedY = nextId++;
                        b13SharedXY = nextId++;
                        b4AppendOp3(rewritten, kB4OpLoad, kB4TypeV3Uint,
                            b13SharedLocalId, kB4LocalInvocationId);
                        b4AppendOp4(rewritten, kB4OpCompositeExtract, kB4TypeUint,
                            b13SharedX, b13SharedLocalId, 0U);
                        b4AppendOp4(rewritten, kB4OpCompositeExtract, kB4TypeUint,
                            b13SharedY, b13SharedLocalId, 1U);
                        b4AppendOp4(rewritten, kB13OpBitwiseOr, kB4TypeUint,
                            b13SharedXY, b13SharedX, b13SharedY);
                        b13CoordinatesEmitted = true;
                    }

                    const uint32_t maskedBits = nextId++;
                    activeLane = nextId++;
                    b4AppendOp4(rewritten, kB11OpBitwiseAnd, kB4TypeUint,
                        maskedBits, b13SharedXY, b11MaskConstants[predicateIndex]);
                    b4AppendOp4(rewritten, kB4OpIEqual, kB4TypeBool,
                        activeLane, maskedBits, kB4Zero);
                } else {
                    const uint32_t localId = nextId++;
                    const uint32_t x = nextId++;
                    const uint32_t y = nextId++;
                    const uint32_t xModulo = nextId++;
                    const uint32_t yModulo = nextId++;
                    const uint32_t xIsZero = nextId++;
                    const uint32_t yIsZero = nextId++;
                    activeLane = nextId++;
                    b4AppendOp3(rewritten, kB4OpLoad, kB4TypeV3Uint,
                        localId, kB4LocalInvocationId);
                    b4AppendOp4(rewritten, kB4OpCompositeExtract, kB4TypeUint,
                        x, localId, 0U);
                    b4AppendOp4(rewritten, kB4OpCompositeExtract, kB4TypeUint,
                        y, localId, 1U);
                    b4AppendOp4(rewritten, kB4OpUMod, kB4TypeUint,
                        xModulo, x, kB4StepConstants[predicateIndex]);
                    b4AppendOp4(rewritten, kB4OpUMod, kB4TypeUint,
                        yModulo, y, kB4StepConstants[predicateIndex]);
                    b4AppendOp4(rewritten, kB4OpIEqual, kB4TypeBool,
                        xIsZero, xModulo, kB4Zero);
                    b4AppendOp4(rewritten, kB4OpIEqual, kB4TypeBool,
                        yIsZero, yModulo, kB4Zero);
                    b4AppendOp4(rewritten, kB4OpLogicalAnd, kB4TypeBool,
                        activeLane, xIsZero, yIsZero);
                }
'''
    text = replace_exact(
        text,
        old_predicate,
        new_predicate,
        count=1,
        label=f"{path}: B13 fused predicate",
    )

    text = replace_exact(
        text,
        "        if (predicateIndex != kB4PredicateCount || pendingPredicate) {\n",
        "        if (predicateIndex != kB4PredicateCount || pendingPredicate\n"
        "                || (b11Enabled && !b13CoordinatesEmitted)) {\n",
        count=1,
        label=f"{path}: B13 completion guard",
    )

    log_anchor = '''        if (b11Enabled) {
            std::cerr << "lsfg-vk: candidate-b11-beta4-pow2-mask shader=" << shaderName
'''
    b13_log = '''        // candidate-b13-beta4-fused-mask
        std::cerr << "lsfg-vk: candidate-b13-beta4-fused-mask shader=" << shaderName
            << " applied=" << (b11Enabled ? 1 : 0)
            << " fused_predicates=" << (b11Enabled ? kB4PredicateCount : 0U)
            << " hoisted_coordinate_sets=" << (b11Enabled ? 4U : 0U)
            << " fallback=" << (b11Enabled ? "none" : "b4") << std::endl;

'''
    text = replace_exact(
        text,
        log_anchor,
        b13_log + log_anchor,
        count=1,
        label=f"{path}: B13 telemetry",
    )

    path.write_text(text, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    patch_translation_source(args.root.resolve() / TRANSLATION_SOURCE)


if __name__ == "__main__":
    main()
