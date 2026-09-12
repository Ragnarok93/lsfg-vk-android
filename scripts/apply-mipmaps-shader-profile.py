#!/usr/bin/env python3
"""Add translated Mipmaps SPIR-V metadata to temporary Android profiling builds.

This transform runs after apply-zero-stage-profile.py. It is intentionally
separate so the already validated zero-stage timing transform remains frozen.
"""

from __future__ import annotations

import argparse
from pathlib import Path


TRANSLATION_HEADER = Path("include/extract/trans.hpp")
TRANSLATION_SOURCE = Path("src/extract/trans.cpp")
LOADER_PATHS = (Path("src/context.cpp"),)
MIPMAP_PATHS = (
    Path("framegen/v3.1_src/shaders/mipmaps.cpp"),
    Path("framegen/v3.1p_src/shaders/mipmaps.cpp"),
)


def replace_exact(text: str, old: str, new: str, *, count: int, label: str) -> str:
    if new in text:
        return text
    found = text.count(old)
    if found != count:
        raise RuntimeError(f"{label}: expected {count} match(es), found {found}")
    return text.replace(old, new, count)


def patch_translation_header(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
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


def spirv_helper() -> str:
    lines = [
        "using namespace Extract;",
        "",
        "namespace {",
        "    constexpr uint32_t kSpirvMagic = 0x07230203U;",
        "    constexpr uint16_t kOpExecutionMode = 16U;",
        "    constexpr uint16_t kOpVariable = 59U;",
        "    constexpr uint32_t kExecutionModeLocalSize = 17U;",
        "    constexpr uint32_t kStorageClassWorkgroup = 4U;",
        "",
        "    uint32_t readSpirvWord(const std::vector<uint8_t>& bytecode, size_t wordIndex) {",
        "        uint32_t word = 0;",
        "        std::memcpy(",
        "            &word,",
        "            bytecode.data() + wordIndex * sizeof(uint32_t),",
        "            sizeof(uint32_t));",
        "        return word;",
        "    }",
        "",
        "    void logMipmapsSpirvProfile(",
        "            const std::string& shaderName,",
        "            const std::vector<uint8_t>& bytecode) {",
        "        if (shaderName != \"mipmaps\" && shaderName != \"p_mipmaps\")",
        "            return;",
        "",
        "        const size_t wordCount = bytecode.size() / sizeof(uint32_t);",
        "        size_t instructionCount = 0;",
        "        size_t workgroupVariables = 0;",
        "        uint32_t localSizeX = 0;",
        "        uint32_t localSizeY = 0;",
        "        uint32_t localSizeZ = 0;",
        "        bool validSpirv = bytecode.size() % sizeof(uint32_t) == 0",
        "            && wordCount >= 5",
        "            && readSpirvWord(bytecode, 0) == kSpirvMagic;",
        "",
        "        if (validSpirv) {",
        "            size_t cursor = 5;",
        "            while (cursor < wordCount) {",
        "                const uint32_t firstWord = readSpirvWord(bytecode, cursor);",
        "                const uint16_t instructionWordCount =",
        "                    static_cast<uint16_t>(firstWord >> 16U);",
        "                const uint16_t opCode = static_cast<uint16_t>(firstWord & 0xffffU);",
        "                if (instructionWordCount == 0",
        "                        || cursor + instructionWordCount > wordCount) {",
        "                    validSpirv = false;",
        "                    break;",
        "                }",
        "",
        "                ++instructionCount;",
        "                if (opCode == kOpExecutionMode && instructionWordCount >= 6) {",
        "                    const uint32_t executionMode = readSpirvWord(bytecode, cursor + 2);",
        "                    if (executionMode == kExecutionModeLocalSize) {",
        "                        localSizeX = readSpirvWord(bytecode, cursor + 3);",
        "                        localSizeY = readSpirvWord(bytecode, cursor + 4);",
        "                        localSizeZ = readSpirvWord(bytecode, cursor + 5);",
        "                    }",
        "                } else if (opCode == kOpVariable && instructionWordCount >= 4) {",
        "                    const uint32_t storageClass = readSpirvWord(bytecode, cursor + 3);",
        "                    if (storageClass == kStorageClassWorkgroup)",
        "                        ++workgroupVariables;",
        "                }",
        "",
        "                cursor += instructionWordCount;",
        "            }",
        "        }",
        "",
        "        std::cerr << \"lsfg-vk: zero-stage-shader-profile\"",
        "            << \" shader=\" << shaderName",
        "            << \" spirv_bytes=\" << bytecode.size()",
        "            << \" spirv_words=\" << wordCount",
        "            << \" instruction_count=\" << instructionCount",
        "            << \" local_size=\" << localSizeX << 'x' << localSizeY << 'x' << localSizeZ",
        "            << \" workgroup_variables=\" << workgroupVariables",
        "            << \" valid_spirv=\" << (validSpirv ? 1 : 0)",
        "            << std::endl;",
        "    }",
        "}",
        "",
    ]
    return "\n".join(lines) + "\n"


def patch_translation_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "zero-stage-shader-profile" in text:
        return

    text = replace_exact(
        text,
        "#include <cstdint>\n#include <cstddef>\n#include <algorithm>\n#include <vector>\n",
        "#include <cstdint>\n#include <cstddef>\n#include <algorithm>\n#include <cstring>\n#include <iostream>\n#include <string>\n#include <vector>\n",
        count=1,
        label=f"{path}: metadata includes",
    )
    text = replace_exact(
        text,
        "using namespace Extract;\n\n",
        spirv_helper(),
        count=1,
        label=f"{path}: SPIR-V metadata helper",
    )
    text = replace_exact(
        text,
        "std::vector<uint8_t> Extract::translateShader(std::vector<uint8_t> bytecode) {\n",
        "std::vector<uint8_t> Extract::translateShader(\n"
        "        std::vector<uint8_t> bytecode, const std::string& shaderName) {\n",
        count=1,
        label=f"{path}: named shader translation signature",
    )
    text = replace_exact(
        text,
        "    std::copy_n(reinterpret_cast<uint8_t*>(code.data()),\n"
        "        code.size(), spirvBytecode.data());\n"
        "    return spirvBytecode;\n",
        "    std::copy_n(reinterpret_cast<uint8_t*>(code.data()),\n"
        "        code.size(), spirvBytecode.data());\n"
        "    logMipmapsSpirvProfile(shaderName, spirvBytecode);\n"
        "    return spirvBytecode;\n",
        count=1,
        label=f"{path}: SPIR-V metadata log",
    )
    path.write_text(text, encoding="utf-8")


def patch_loader(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    text = replace_exact(
        text,
        "Extract::translateShader(dxbc)",
        "Extract::translateShader(dxbc, name)",
        count=2,
        label=f"{path}: shader-name propagation",
    )
    path.write_text(text, encoding="utf-8")


def patch_mipmaps_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    text = replace_exact(
        text,
        "        << \" flow_scale=\" << vk.flowScale\n"
        "        << \" flow_extent=\" << flowExtent.width << 'x' << flowExtent.height\n",
        "        << \" flow_scale=\" << vk.flowScale\n"
        "        << \" effective_flow_scale=\" << (1.0F / vk.flowScale)\n"
        "        << \" flow_extent=\" << flowExtent.width << 'x' << flowExtent.height\n",
        count=1,
        label=f"{path}: effective flow scale",
    )
    path.write_text(text, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="repository root already patched by apply-zero-stage-profile.py",
    )
    args = parser.parse_args()
    root = args.root.resolve()

    patch_translation_header(root / TRANSLATION_HEADER)
    patch_translation_source(root / TRANSLATION_SOURCE)
    for rel in LOADER_PATHS:
        patch_loader(root / rel)
    for rel in MIPMAP_PATHS:
        patch_mipmaps_source(root / rel)


if __name__ == "__main__":
    main()
