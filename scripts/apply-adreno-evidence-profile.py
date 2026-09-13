#!/usr/bin/env python3
"""Inject bundled Adreno evidence profiling plus the SYNC_FD validation fast path."""
from __future__ import annotations

import argparse
from pathlib import Path
from adreno_evidence_capabilities import (patch_backend_header, patch_device_source, patch_hooks_header, patch_hooks_source)
from adreno_evidence_outer import patch_outer_header, patch_outer_source
from adreno_evidence_framegen import patch_framegen_header, patch_framegen_source, patch_timestamp_query_pool
from adreno_syncfd_handoff import apply as apply_syncfd_handoff

FRAMEGEN_HEADERS = (
    Path("framegen/v3.1_include/v3_1/context.hpp"),
    Path("framegen/v3.1p_include/v3_1p/context.hpp"),
)
FRAMEGEN_SOURCES = (
    (Path("framegen/v3.1_src/context.cpp"), "quality"),
    (Path("framegen/v3.1p_src/context.cpp"), "performance"),
)

def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    root = args.root.resolve()
    patch_backend_header(root / "framegen/public/lsfg_backend.hpp")
    patch_device_source(root / "framegen/src/core/device.cpp")
    patch_hooks_header(root / "include/hooks.hpp")
    patch_hooks_source(root / "src/hooks.cpp")
    patch_outer_header(root / "include/context.hpp")
    patch_outer_source(root / "src/context.cpp")
    patch_timestamp_query_pool(
        root / "framegen/include/core/timestampquerypool.hpp",
        root / "framegen/src/core/timestampquerypool.cpp",
    )
    for rel in FRAMEGEN_HEADERS:
        patch_framegen_header(root / rel)
    for rel, backend in FRAMEGEN_SOURCES:
        patch_framegen_source(root / rel, backend)
    apply_syncfd_handoff(root)

if __name__ == "__main__":
    main()
