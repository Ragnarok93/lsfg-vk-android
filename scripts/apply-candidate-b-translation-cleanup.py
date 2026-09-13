#!/usr/bin/env python3
"""Apply Candidate B lossless translation cleanup to Android builds.

This transform intentionally runs after optional shader profiling transforms so
those already-validated transforms can keep matching the original source. It is
safe in profiling and non-profiling builds and changes only DXBC translation for
p_mipmaps and p_beta[4].
"""
from __future__ import annotations

import argparse
from pathlib import Path


TRANSLATION_HEADER = Path("include/extract/trans.hpp")
TRANSLATION_SOURCE = Path("src/extract/trans.cpp")
CONTEXT_SOURCE = Path("src/context.cpp")


def replace_exact(text: str, old: str, new: str, *, count: int, label: str) -> str:
    found = text.count(old)
    if found != count:
        raise RuntimeError(f"{label}: expected {count} match(es), found {found}")
    return text.replace(old, new, count)


def patch_header(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "const std::string& shaderName" in text:
        path.write_text(text, encoding="utf-8")
        return

    if "#include <string>" not in text:
        text = replace_exact(
            text,
            "#include <cstdint>\n#include <vector>\n",
            "#include <cstdint>\n#include <string>\n#include <vector>\n",
            count=1,
            label=f"{path}: string include",
        )

    text = replace_exact(
        text,
        "    std::vector<uint8_t> translateShader(std::vector<uint8_t> bytecode);\n",
        "    std::vector<uint8_t> translateShader(\n"
        "        std::vector<uint8_t> bytecode, const std::string& shaderName = {});\n",
        count=1,
        label=f"{path}: named shader translation",
    )
    path.write_text(text, encoding="utf-8")


def patch_translator(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "shader-hot-path-opt" in text:
        return

    if "#include <iostream>" not in text:
        insert_after = "#include <algorithm>\n"
        if insert_after not in text:
            raise RuntimeError(f"{path}: algorithm include anchor missing")
        text = text.replace(
            insert_after,
            insert_after + "#include <iostream>\n#include <string>\n",
            1,
        )
    elif "#include <string>" not in text:
        text = text.replace("#include <iostream>\n", "#include <iostream>\n#include <string>\n", 1)

    old_signature = "std::vector<uint8_t> Extract::translateShader(std::vector<uint8_t> bytecode) {\n"
    if old_signature in text:
        text = text.replace(
            old_signature,
            "std::vector<uint8_t> Extract::translateShader(\n"
            "        std::vector<uint8_t> bytecode, const std::string& shaderName) {\n",
            1,
        )
    elif "const std::string& shaderName" not in text:
        raise RuntimeError(f"{path}: named translation signature unavailable")

    old_info = "    const dxvk::DxbcModuleInfo info{};\n    auto code = module.compile(info, \"CS\");\n"
    new_info = (
        "    dxvk::DxbcModuleInfo info{};\n"
        "    const bool optimizeHotPath = shaderName == \"p_mipmaps\"\n"
        "        || shaderName == \"p_beta[4]\";\n"
        "    if (optimizeHotPath) {\n"
        "        info.options.supportsTightIcbPacking = true;\n"
        "        std::cerr << \"lsfg-vk: shader-hot-path-opt shader=\" << shaderName\n"
        "                  << \" tight_icb=1\" << std::endl;\n"
        "    }\n"
        "    auto code = module.compile(info, \"CS\");\n"
    )
    text = replace_exact(
        text,
        old_info,
        new_info,
        count=1,
        label=f"{path}: targeted tight ICB option",
    )
    path.write_text(text, encoding="utf-8")


def patch_android_loader(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    # Profiling transforms already propagate shader names to both loader copies.
    # In a normal Android build, update only the first occurrence, which is the
    # Android loader. The desktop source path remains byte-for-byte unchanged.
    if "Extract::translateShader(dxbc, name)" in text:
        return

    old = "auto spirv = Extract::translateShader(dxbc);"
    if text.count(old) < 2:
        raise RuntimeError(f"{path}: expected Android and desktop shader loaders")
    text = text.replace(old, "auto spirv = Extract::translateShader(dxbc, name);", 1)
    path.write_text(text, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    root = args.root.resolve()

    patch_header(root / TRANSLATION_HEADER)
    patch_translator(root / TRANSLATION_SOURCE)
    patch_android_loader(root / CONTEXT_SOURCE)


if __name__ == "__main__":
    main()
