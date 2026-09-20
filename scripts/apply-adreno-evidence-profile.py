#!/usr/bin/env python3
"""Apply only retained, source-compatible Android capability diagnostics.

The restored Adaptive Flow Scale baseline intentionally does not compose the
DeferredZero, async-zero-history, transport-overlap, SYNC_FD handoff, or
nonblocking-generated experimental stacks at build time. Individual mechanisms
must be promoted into source explicitly, with focused tests, before production
builds are allowed to use them.
"""
from __future__ import annotations

import argparse
from pathlib import Path

from adreno_evidence_capabilities import (
    patch_backend_header,
    patch_device_source,
    patch_hooks_header,
    patch_hooks_source,
)


def apply_runtime(root: Path) -> None:
    """Apply diagnostic-only capability probing to the restored source."""
    patch_backend_header(root / "framegen/public/lsfg_backend.hpp")
    patch_device_source(root / "framegen/src/core/device.cpp")
    patch_hooks_header(root / "include/hooks.hpp")
    patch_hooks_source(root / "src/hooks.cpp")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    apply_runtime(args.root.resolve())


if __name__ == "__main__":
    main()
