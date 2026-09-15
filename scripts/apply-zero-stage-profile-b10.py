#!/usr/bin/env python3
"""Compose zero-stage profiling with the B10 split Mipmaps dispatch.

The validated zero-stage profiler still patches the normal Mipmaps fallback.
This wrapper adds the same post-input-barrier timestamp to the B10 head path so
Mipmaps timing includes head + inter-dispatch dependency + compact tail.
"""
from __future__ import annotations

import argparse
import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASE_PATH = ROOT / "scripts/apply-zero-stage-profile.py"

spec = importlib.util.spec_from_file_location("zero_stage_profile_base", BASE_PATH)
if spec is None or spec.loader is None:
    raise RuntimeError(f"unable to load {BASE_PATH}")
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)


def patch_b10_performance_mipmaps(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    marker = "b10-zero-stage-post-head-barrier"
    if marker in text:
        return
    old = (
        "        preHead.build();\n\n"
        "        this->pipeline.bind(buf);\n"
    )
    new = (
        "        preHead.build();\n"
        "        // b10-zero-stage-post-head-barrier: query 1 begins B10 GPU work.\n"
        "        if (profilePool != nullptr && profilePool->supported())\n"
        "            profilePool->write(buf.handle(), postBarrierQueryIndex);\n\n"
        "        this->pipeline.bind(buf);\n"
    )
    path.write_text(base.replace_exact(
        text, old, new, count=1, label=f"{path}: B10 post-barrier timestamp"),
        encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args()
    root = args.root.resolve()

    for rel in base.HEADER_PATHS:
        base.patch_header(root / rel)
    for header_rel, source_rel, backend in base.MIPMAP_PATHS:
        base.patch_mipmaps_header(root / header_rel)
        base.patch_mipmaps_source(root / source_rel, backend)
        if backend == "performance":
            patch_b10_performance_mipmaps(root / source_rel)
    for rel, backend in base.SOURCE_PATHS:
        base.patch_source(root / rel, backend)


if __name__ == "__main__":
    main()
