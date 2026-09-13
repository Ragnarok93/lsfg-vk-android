#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path

from adreno_evidence_common import replace_exact


BACKENDS = (
    Path("framegen/v3.1_src/lsfg.cpp"),
    Path("framegen/v3.1p_src/lsfg.cpp"),
)


def patch_backend(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "framegen runtime retained after last Android context" in text:
        return

    old = """    contexts.erase(it);\n    if (contexts.empty())\n        resetRuntime();\n}\n"""
    new = """    contexts.erase(it);\n#ifndef __ANDROID__\n    if (contexts.empty())\n        resetRuntime();\n#else\n    // Android swapchain/context churn must not unload the private Vulkan runtime\n    // from deleteContext() while runtimeMutex is held. Reconfiguration and the\n    // explicit finalize() path remain the only runtime destruction boundaries.\n    if (contexts.empty())\n        std::cerr << \"lsfg-vk: framegen runtime retained after last Android context\\n\";\n#endif\n}\n"""
    text = replace_exact(
        text,
        old,
        new,
        count=1,
        label=f"{path}: keep Android private runtime resident",
    )
    path.write_text(text, encoding="utf-8")


def apply(root: Path) -> None:
    for relative in BACKENDS:
        patch_backend(root / relative)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    apply(args.root.resolve())


if __name__ == "__main__":
    main()
