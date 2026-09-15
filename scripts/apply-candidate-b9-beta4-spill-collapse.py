#!/usr/bin/env python3
"""Apply Candidate B9: exact Beta4 phase-0 spill-collapse scheduling.

B9 runs after B4 and injects a fail-closed SPIR-V list scheduler.  The helper
keeps the exact B4 module graph and only reschedules dependency-safe phase-0
instructions so each sampled vector is consumed as soon as possible.
"""
from __future__ import annotations

import argparse
from pathlib import Path

TRANSLATION_SOURCE = Path("src/extract/trans.cpp")
HELPER_DIR = Path(__file__).resolve().parent / "b9_beta4_spill_collapse"
HELPER_PARTS = ("part1.inc", "part2.inc", "part3.inc")
MARKER = "candidate-b9-beta4-spill-collapse"


def replace_exact(text: str, old: str, new: str, *, count: int, label: str) -> str:
    if new in text:
        return text
    found = text.count(old)
    if found != count:
        raise RuntimeError(f"{label}: expected {count} match(es), found {found}")
    return text.replace(old, new, count)


def helper() -> str:
    return "".join(
        (HELPER_DIR / part).read_text(encoding="utf-8")
        for part in HELPER_PARTS
    )


def patch_translation_source(root: Path, path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if MARKER in text:
        return

    b4_call = "    applyCandidateB4Beta4PredicateCanonicalization(shaderName, spirvBytecode);\n"
    if b4_call not in text:
        raise RuntimeError(f"{path}: Candidate B9 requires Candidate B4 to be applied first")

    extra_includes = []
    for header in ("algorithm", "array", "unordered_map", "unordered_set", "utility", "vector"):
        token = f"#include <{header}>"
        if token not in text:
            extra_includes.append(token + "\n")
    if extra_includes:
        text = replace_exact(
            text,
            "#include <cstddef>\n",
            "#include <cstddef>\n" + "".join(extra_includes),
            count=1,
            label=f"{path}: B9 includes",
        )

    text = replace_exact(
        text,
        "struct BindingOffsets {\n",
        helper() + "struct BindingOffsets {\n",
        count=1,
        label=f"{path}: B9 helper",
    )
    text = replace_exact(
        text,
        b4_call,
        b4_call + "    applyCandidateB9Beta4SpillCollapse(shaderName, spirvBytecode);\n",
        count=1,
        label=f"{path}: B9 invocation",
    )
    path.write_text(text, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    root = args.root.resolve()
    patch_translation_source(root, root / TRANSLATION_SOURCE)


if __name__ == "__main__":
    main()
