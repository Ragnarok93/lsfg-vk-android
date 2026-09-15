#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path

TARGET = Path("framegen/src/pool/shaderpool.cpp")
MARKER = "kB8OpExtInst"

CONSTANTS = '''constexpr uint16_t kB8OpExtInst = 12U;\nconstexpr uint16_t kB8OpExecutionMode = 16U;\nconstexpr uint16_t kB8OpCopyObject = 83U;\nconstexpr uint16_t kB8OpImageWrite = 99U;\nconstexpr uint16_t kB8OpFMul = 133U;\nconstexpr uint16_t kB8OpControlBarrier = 224U;\nconstexpr uint32_t kB8ExecutionModeLocalSize = 17U;\n'''

REPLACEMENTS = {
    "static_cast<uint16_t>(spv::OpExtInst)": "kB8OpExtInst",
    "static_cast<uint16_t>(spv::OpExecutionMode)": "kB8OpExecutionMode",
    "static_cast<uint32_t>(spv::OpExtInst)": "static_cast<uint32_t>(kB8OpExtInst)",
    "static_cast<uint32_t>(spv::OpCopyObject)": "static_cast<uint32_t>(kB8OpCopyObject)",
    "static_cast<uint16_t>(spv::OpImageWrite)": "kB8OpImageWrite",
    "static_cast<uint16_t>(spv::OpFMul)": "kB8OpFMul",
    "static_cast<uint16_t>(spv::OpControlBarrier)": "kB8OpControlBarrier",
    "static_cast<uint32_t>(spv::ExecutionModeLocalSize)": "kB8ExecutionModeLocalSize",
}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    path = args.root.resolve() / TARGET
    text = path.read_text(encoding="utf-8")

    if MARKER in text and "thirdparty/spirv.hpp" not in text and "spv::" not in text:
        return

    include = "#include <thirdparty/spirv.hpp>\n"
    if include not in text:
        raise RuntimeError("B8 SPIR-V include anchor not found")
    text = text.replace(include, "", 1)

    anchor = "namespace {\nconstexpr uint64_t kB8ExpectedFnv"
    if anchor not in text:
        raise RuntimeError("B8 helper namespace anchor not found")
    text = text.replace(
        anchor,
        "namespace {\n" + CONSTANTS + "constexpr uint64_t kB8ExpectedFnv",
        1,
    )

    for old, new in REPLACEMENTS.items():
        if old not in text:
            raise RuntimeError(f"B8 SPIR-V token not found: {old}")
        text = text.replace(old, new)

    if "spv::" in text or "thirdparty/spirv.hpp" in text:
        raise RuntimeError("B8 SPIR-V dependency cleanup incomplete")

    path.write_text(text, encoding="utf-8")


if __name__ == "__main__":
    main()
