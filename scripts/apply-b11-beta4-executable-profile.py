#!/usr/bin/env python3
"""Extend B6 pipeline-executable diagnostics to p_beta[4] for B11 evidence builds.

This is a profiling-only post-transform. It requires Candidate B6 to have
already added VK_KHR_pipeline_executable_properties support and then broadens
the capture predicate/logging from p_mipmaps to p_mipmaps + p_beta[4]. Clean
runtime builds and ordinary B6/Mipmaps diagnostics are unchanged.
"""
from __future__ import annotations

import argparse
from pathlib import Path

PIPELINE_CPP = Path("framegen/src/core/pipeline.cpp")
MARKER = "b11-beta4-executable-profile"


def replace_once(text: str, old: str, new: str, *, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected one match, found {count}")
    return text.replace(old, new, 1)


def patch_pipeline_cpp(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if MARKER in text:
        return

    if "pipeline-exec-profile shader=p_mipmaps" not in text:
        raise RuntimeError(f"{path}: Candidate B6 executable profiling must be applied first")

    text = replace_once(
        text,
        '    if (shaderName != "p_mipmaps") return;\n',
        '    // b11-beta4-executable-profile: evidence builds also capture p_beta[4].\n'
        '    if (shaderName != "p_mipmaps" && shaderName != "p_beta[4]") return;\n',
        label=f"{path}: executable helper shader gate",
    )

    text = replace_once(
        text,
        '    const bool captureExecutableInfo = shaderName == "p_mipmaps"\n'
        '        && device.supportsPipelineExecutableProperties();\n',
        '    const bool captureExecutableInfo =\n'
        '        (shaderName == "p_mipmaps" || shaderName == "p_beta[4]")\n'
        '        && device.supportsPipelineExecutableProperties();\n',
        label=f"{path}: executable capture predicate",
    )

    # B6 deliberately hard-codes p_mipmaps in its diagnostic labels because it
    # normally captures only that shader. Evidence builds need unambiguous
    # Beta4 output, so make those labels use the canonical shaderName.
    label_count = text.count("shader=p_mipmaps")
    if label_count < 6:
        raise RuntimeError(
            f"{path}: expected B6 diagnostic labels, found only {label_count}"
        )
    text = text.replace("shader=p_mipmaps", 'shader=" << shaderName << "')

    # Keep B6's idempotence marker discoverable if a diagnostic composition
    # accidentally invokes B6 again after this post-transform.
    text = text.replace(
        "// b11-beta4-executable-profile: evidence builds also capture p_beta[4].\n",
        "// b11-beta4-executable-profile: evidence builds also capture p_beta[4].\n"
        "// pipeline-exec-profile shader=p_mipmaps (B6 marker retained)\n",
        1,
    )
    path.write_text(text, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    patch_pipeline_cpp(args.root.resolve() / PIPELINE_CPP)


if __name__ == "__main__":
    main()
