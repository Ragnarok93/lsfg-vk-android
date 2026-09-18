#!/usr/bin/env python3
"""Compose retained Android runtime transforms for production builds."""
from __future__ import annotations

import argparse
from pathlib import Path
from adreno_evidence_capabilities import (
    patch_backend_header,
    patch_device_source,
    patch_hooks_header,
    patch_hooks_source,
)
from adreno_evidence_common import replace_exact
from adreno_syncfd_handoff import apply as apply_syncfd_handoff

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


def apply_runtime(root: Path) -> None:
    """Apply retained Android runtime behavior without GPU/compiler profilers."""
    patch_backend_header(root / "framegen/public/lsfg_backend.hpp")
    patch_device_source(root / "framegen/src/core/device.cpp")
    patch_hooks_header(root / "include/hooks.hpp")
    patch_hooks_source(root / "src/hooks.cpp")

    mini_semaphore_header = root / "include/mini/semaphore.hpp"
    if "int exportFd(" not in mini_semaphore_header.read_text(encoding="utf-8"):
        apply_syncfd_handoff(root)
    normalize_sync_fd_import_initializer(root / "framegen/src/core/semaphore.cpp")

    # Source-protected fractional FG deliberately stays on the restored
    # presentContextWithCount(..., 0) history-maintenance path. Do not compose
    # the discarded asynchronous/DeferredZero transform stacks here: they add
    # a second history controller and rewrite the framegen API/lifetime model
    # underneath the source-protected scheduler.


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    apply_runtime(args.root.resolve())


if __name__ == "__main__":
    main()
