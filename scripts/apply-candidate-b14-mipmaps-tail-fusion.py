#!/usr/bin/env python3
"""Wire the checked B14 Mipmaps tail fusion into translated p_mipmaps."""

from __future__ import annotations

import argparse
from pathlib import Path


TRANSLATION_SOURCE = Path("src/extract/trans.cpp")
MARKER = "candidate-b14-mipmaps-tail-fusion"


def replace_exact(text: str, old: str, new: str, *, count: int, label: str) -> str:
    if new in text:
        return text
    found = text.count(old)
    if found != count:
        raise RuntimeError(f"{label}: expected {count} match(es), found {found}")
    return text.replace(old, new, count)


def patch_translation_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if MARKER in text:
        return
    if "const std::string& shaderName" not in text:
        raise RuntimeError(f"{path}: B14 requires named shader translation")

    text = replace_exact(
        text,
        '#include "extract/trans.hpp"\n',
        '#include "extract/trans.hpp"\n'
        '#include "../../scripts/b14_mipmaps_tail_fusion.hpp"\n',
        count=1,
        label=f"{path}: B14 rewriter include",
    )
    if "#include <iostream>\n" not in text:
        text = replace_exact(
            text,
            "#include <vector>\n",
            "#include <iostream>\n#include <vector>\n",
            count=1,
            label=f"{path}: B14 log include",
        )
    if "#include <stdexcept>\n" not in text:
        text = replace_exact(
            text,
            "#include <vector>\n",
            "#include <stdexcept>\n#include <vector>\n",
            count=1,
            label=f"{path}: B14 exception include",
        )

    call = '''    if (shaderName == "p_mipmaps") {
        const auto b14Report = b14::fuseTail(spirvBytecode);
        if (!b14Report.applied && !b14Report.alreadyApplied)
            throw std::runtime_error(
                std::string("B14 Mipmaps tail fusion rejected known shader: ")
                + b14Report.reason);
        std::cerr << "lsfg-vk: candidate-b14-mipmaps-tail-fusion"
                  << " applied=" << (b14Report.applied ? 1 : 0)
                  << " already_applied=" << (b14Report.alreadyApplied ? 1 : 0)
                  << " barriers_before=" << b14Report.barriersBefore
                  << " barriers_after=" << b14Report.barriersAfter
                  << " tail_dynamic_loads_before=" << b14Report.tailDynamicLoadsBefore
                  << " tail_dynamic_loads_after=" << b14Report.tailDynamicLoadsAfter
                  << " tail_workgroup_stores_before=" << b14Report.tailWorkgroupStoresBefore
                  << " tail_workgroup_stores_after=" << b14Report.tailWorkgroupStoresAfter
                  << " image_writes_before=" << b14Report.imageWritesBefore
                  << " image_writes_after=" << b14Report.imageWritesAfter
                  << std::endl;
    }
'''
    text = replace_exact(
        text,
        "    return spirvBytecode;\n",
        call + "    return spirvBytecode;\n",
        count=1,
        label=f"{path}: B14 translation call",
    )
    path.write_text(text, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    patch_translation_source(args.root.resolve() / TRANSLATION_SOURCE)


if __name__ == "__main__":
    main()
