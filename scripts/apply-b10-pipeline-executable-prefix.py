#!/usr/bin/env python3
"""Extend B6 pipeline-executable capture to the B10 Mipmaps tail.

Run only after apply-candidate-b6-pipeline-executable-profile.py. The B6 helper
uses the stable p_mipmaps label in its detailed lines; this patch adds an
explicit begin marker with the real shader key so head/tail blocks remain
unambiguous without rewriting the validated B6 diagnostics.
"""
from __future__ import annotations

import argparse
from pathlib import Path

PIPELINE_CPP = Path("framegen/src/core/pipeline.cpp")
MARKER = "candidate-b10-pipeline-profile shader="


def replace_exact(text: str, old: str, new: str, label: str) -> str:
    if new in text:
        return text
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected one anchor, found {count}")
    return text.replace(old, new, 1)


def patch(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if MARKER in text:
        return
    text = replace_exact(
        text,
        '    if (shaderName != "p_mipmaps") return;\n',
        '    if (shaderName.rfind("p_mipmaps", 0) != 0) return;\n'
        '    std::cerr << "lsfg-vk: candidate-b10-pipeline-profile shader="\n'
        '              << shaderName << \'\\n\';\n',
        "B6 profile shader prefix",
    )
    text = replace_exact(
        text,
        '    const bool captureExecutableInfo = shaderName == "p_mipmaps"\n'
        '        && device.supportsPipelineExecutableProperties();\n',
        '    const bool captureExecutableInfo = shaderName.rfind("p_mipmaps", 0) == 0\n'
        '        && device.supportsPipelineExecutableProperties();\n',
        "B6 capture shader prefix",
    )
    path.write_text(text, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    patch(args.root.resolve() / PIPELINE_CPP)


if __name__ == "__main__":
    main()
