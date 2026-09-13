#!/usr/bin/env python3
"""Inject bundled Adreno evidence profiling plus the SYNC_FD validation fast path."""
from __future__ import annotations

import argparse
from pathlib import Path
from adreno_evidence_capabilities import (patch_backend_header, patch_device_source, patch_hooks_header, patch_hooks_source)
from adreno_evidence_outer import patch_outer_header, patch_outer_source
from adreno_evidence_framegen import patch_framegen_header, patch_framegen_source, patch_timestamp_query_pool
from adreno_evidence_common import replace_exact
from adreno_syncfd_handoff import apply as apply_syncfd_handoff
from adreno_async_zero_history import apply as apply_async_zero_history
from adreno_async_zero_history_hardening import apply as apply_async_zero_history_hardening
from adreno_slot_aware_zero_history import apply as apply_slot_aware_zero_history
from adreno_transport_release_overlap import apply as apply_transport_release_overlap
from adreno_deferred_zero_history_build import apply as apply_deferred_zero_history
from adreno_deferred_zero_history_finalize import apply as apply_deferred_zero_history_finalize

FRAMEGEN_HEADERS = (
    Path("framegen/v3.1_include/v3_1/context.hpp"),
    Path("framegen/v3.1p_include/v3_1p/context.hpp"),
)
FRAMEGEN_SOURCES = (
    (Path("framegen/v3.1_src/context.cpp"), "quality"),
    (Path("framegen/v3.1p_src/context.cpp"), "performance"),
)


def normalize_sync_fd_import_initializer(path: Path) -> None:
    """Keep the validation transform valid under Android NDK C++20 rules."""
    text = path.read_text(encoding="utf-8")
    invalid = '''        const VkImportSemaphoreFdInfoKHR importInfo{
            .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
            .flags = handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT
                ? VK_SEMAPHORE_IMPORT_TEMPORARY_BIT : 0,
            .semaphore = semaphoreHandle,
            .handleType = handleType,
            .fd = fd,
        };
'''
    if invalid not in text:
        return
    corrected = '''        const VkImportSemaphoreFdInfoKHR importInfo{
            .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
            .semaphore = semaphoreHandle,
            .flags = handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT
                ? static_cast<VkSemaphoreImportFlags>(VK_SEMAPHORE_IMPORT_TEMPORARY_BIT)
                : VkSemaphoreImportFlags{0},
            .handleType = handleType,
            .fd = fd,
        };
'''
    text = replace_exact(
        text,
        invalid,
        corrected,
        count=1,
        label=f"{path}: C++20-valid SYNC_FD import initializer",
    )
    path.write_text(text, encoding="utf-8")


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
    mini_semaphore_header = root / "include/mini/semaphore.hpp"
    if "int exportFd(" not in mini_semaphore_header.read_text(encoding="utf-8"):
        apply_syncfd_handoff(root)
    normalize_sync_fd_import_initializer(root / "framegen/src/core/semaphore.cpp")
    apply_async_zero_history(root)
    apply_async_zero_history_hardening(root)
    apply_slot_aware_zero_history(root)
    apply_transport_release_overlap(root)
    # Synthetic evidence-bundle fixtures intentionally contain only the files
    # touched by that test. Full Android builds always contain Mini::Image and
    # therefore always apply Candidate A here.
    if (root / "include/mini/image.hpp").exists():
        apply_deferred_zero_history(root)
        apply_deferred_zero_history_finalize(root)


if __name__ == "__main__":
    main()
