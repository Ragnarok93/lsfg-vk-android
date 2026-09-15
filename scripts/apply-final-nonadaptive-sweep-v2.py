#!/usr/bin/env python3
"""Composition-safe wrapper for the final non-adaptive Adreno sweep.

Reuses the validated probe builders/pipeline changes from
apply-final-nonadaptive-sweep.py, but places the deferred trigger at a boundary
that survives the earlier async zero-history/profile transforms.
"""
from __future__ import annotations

import argparse
import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASE_PATH = ROOT / "scripts/apply-final-nonadaptive-sweep.py"

spec = importlib.util.spec_from_file_location("final_nonadaptive_sweep_base", BASE_PATH)
if spec is None or spec.loader is None:
    raise RuntimeError(f"unable to load {BASE_PATH}")
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)


def patch_context(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "kAdrenoSweepWarmupFrames" in text:
        return

    text = base.replace_exact(
        text,
        "namespace {\nuint64_t framegenWaitTimeoutNs() {\n",
        "namespace {\n"
        "constexpr uint64_t kAdrenoSweepWarmupFrames = 120U;\n"
        "constexpr uint64_t kAdrenoSweepIntervalFrames = 45U;\n"
        "uint64_t framegenWaitTimeoutNs() {\n",
        "Context sweep constants",
    )

    # Earlier async zero-history transforms insert preprocessing-retirement work
    # immediately after the data lookup.  The wait boundary remains stable and
    # intentionally places probe compilation after that retirement work but
    # before the normal per-slot completion wait.
    anchor = "    if (data.shouldWait)\n"
    insertion = (
        "#ifdef __ANDROID__\n"
        "    if (this->frameIdx >= kAdrenoSweepWarmupFrames\n"
        "            && ((this->frameIdx - kAdrenoSweepWarmupFrames)\n"
        "                % kAdrenoSweepIntervalFrames) == 0U)\n"
        "        vk.shaders.runNextAdrenoSweepProbe(vk.device);\n"
        "#endif\n\n"
        + anchor
    )
    text = base.replace_exact(text, anchor, insertion, "Context deferred probe trigger")
    path.write_text(text, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args()
    root = args.root.resolve()

    base.patch_pipeline(root / base.PIPELINE_CPP)
    base.patch_pool_hpp(root / base.POOL_HPP)
    base.patch_pool_cpp(root / base.POOL_CPP)
    patch_context(root / base.CONTEXT_CPP)


if __name__ == "__main__":
    main()
