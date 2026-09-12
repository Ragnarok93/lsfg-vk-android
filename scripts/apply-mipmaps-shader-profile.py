#!/usr/bin/env python3
"""Add translated Mipmaps SPIR-V structure metadata to Android profiling builds.

This transform runs after apply-zero-stage-profile.py. It is intentionally
separate so the already validated zero-stage timing transform remains frozen.
It never mutates shader bytecode: it only inspects and logs the translated
SPIR-V used by Mipmaps.
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
        "    constexpr uint16_t kOpExecutionMode = static_cast<uint16_t>(spv::OpExecutionMode);",
        "    constexpr uint16_t kOpTypeBool = static_cast<uint16_t>(spv::OpTypeBool);",
        "    constexpr uint16_t kOpTypeInt = static_cast<uint16_t>(spv::OpTypeInt);",
        "    constexpr uint16_t kOpTypeFloat = static_cast<uint16_t>(spv::OpTypeFloat);",
        "    constexpr uint16_t kOpTypeVector = static_cast<uint16_t>(spv::OpTypeVector);",
        "    constexpr uint16_t kOpTypeMatrix = static_cast<uint16_t>(spv::OpTypeMatrix);",
        "    constexpr uint16_t kOpTypeArray = static_cast<uint16_t>(spv::OpTypeArray);",
        "    constexpr uint16_t kOpTypeRuntimeArray = static_cast<uint16_t>(spv::OpTypeRuntimeArray);",
        "    constexpr uint16_t kOpTypeStruct = static_cast<uint16_t>(spv::OpTypeStruct);",
        "    constexpr uint16_t kOpTypePointer = static_cast<uint16_t>(spv::OpTypePointer);",
        "    constexpr uint16_t kOpConstant = static_cast<uint16_t>(spv::OpConstant);",
        "    constexpr uint16_t kOpVariable = static_cast<uint16_t>(spv::OpVariable);",
        "    constexpr uint16_t kOpDecorate = static_cast<uint16_t>(spv::OpDecorate);",
        "    constexpr uint16_t kOpControlBarrier = static_cast<uint16_t>(spv::OpControlBarrier);",
        "    constexpr uint16_t kOpMemoryBarrier = static_cast<uint16_t>(spv::OpMemoryBarrier);",
        "    constexpr uint16_t kOpImageSampleImplicitLod = static_cast<uint16_t>(spv::OpImageSampleImplicitLod);",
        "    constexpr uint16_t kOpImageSampleExplicitLod = static_cast<uint16_t>(spv::OpImageSampleExplicitLod);",
        "    constexpr uint16_t kOpImageSampleDrefImplicitLod = static_cast<uint16_t>(spv::OpImageSampleDrefImplicitLod);",
        "    constexpr uint16_t kOpImageSampleDrefExplicitLod = static_cast<uint16_t>(spv::OpImageSampleDrefExplicitLod);",
        "    constexpr uint16_t kOpImageSampleProjImplicitLod = static_cast<uint16_t>(spv::OpImageSampleProjImplicitLod);",
        "    constexpr uint16_t kOpImageSampleProjExplicitLod = static_cast<uint16_t>(spv::OpImageSampleProjExplicitLod);",
        "    constexpr uint16_t kOpImageSampleProjDrefImplicitLod = static_cast<uint16_t>(spv::OpImageSampleProjDrefImplicitLod);",
        "    constexpr uint16_t kOpImageSampleProjDrefExplicitLod = static_cast<uint16_t>(spv::OpImageSampleProjDrefExplicitLod);",
        "    constexpr uint16_t kOpImageFetch = static_cast<uint16_t>(spv::OpImageFetch);",
        "    constexpr uint16_t kOpImageGather = static_cast<uint16_t>(spv::OpImageGather);",
        "    constexpr uint16_t kOpImageDrefGather = static_cast<uint16_t>(spv::OpImageDrefGather);",
        "    constexpr uint16_t kOpImageRead = static_cast<uint16_t>(spv::OpImageRead);",
        "    constexpr uint16_t kOpImageWrite = static_cast<uint16_t>(spv::OpImageWrite);",
        "    constexpr uint32_t kExecutionModeLocalSize = static_cast<uint32_t>(spv::ExecutionModeLocalSize);",
        "    constexpr uint32_t kStorageClassWorkgroup = static_cast<uint32_t>(spv::StorageClassWorkgroup);",
        "    constexpr uint32_t kDecorationArrayStride = static_cast<uint32_t>(spv::DecorationArrayStride);",
        "    constexpr uint32_t kDecorationBuiltIn = static_cast<uint32_t>(spv::DecorationBuiltIn);",
        "    constexpr uint32_t kBuiltInWorkgroupId = static_cast<uint32_t>(spv::BuiltInWorkgroupId);",
        "    constexpr uint32_t kBuiltInLocalInvocationId = static_cast<uint32_t>(spv::BuiltInLocalInvocationId);",
        "    constexpr size_t kSpirvDumpBytesPerChunk = 512;",
        "",
        "    struct SpirvTypeInfo {",
        "        uint16_t opCode{};",
        "        uint32_t widthBits{};",
        "        uint32_t elementType{};",
        "        uint32_t elementCount{};",
        "        uint32_t lengthId{};",
        "        uint32_t storageClass{};",
        "        std::vector<uint32_t> memberTypes;",
        "    };",
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
        "    uint64_t checkedMul(uint64_t lhs, uint64_t rhs) {",
        "        if (lhs == 0 || rhs == 0)",
        "            return 0;",
        "        if (lhs > std::numeric_limits<uint64_t>::max() / rhs)",
        "            return 0;",
        "        return lhs * rhs;",
        "    }",
        "",
        "    uint64_t spirvTypeSizeBytes(",
        "            uint32_t typeId,",
        "            const std::unordered_map<uint32_t, SpirvTypeInfo>& types,",
        "            const std::unordered_map<uint32_t, uint64_t>& constants,",
        "            const std::unordered_map<uint32_t, uint32_t>& arrayStrides,",
        "            std::unordered_set<uint32_t>& resolving) {",
        "        if (!resolving.insert(typeId).second)",
        "            return 0;",
        "        const auto finish = [&resolving, typeId](uint64_t value) {",
        "            resolving.erase(typeId);",
        "            return value;",
        "        };",
        "",
        "        const auto typeIt = types.find(typeId);",
        "        if (typeIt == types.end())",
        "            return finish(0);",
        "        const SpirvTypeInfo& type = typeIt->second;",
        "        switch (type.opCode) {",
        "        case kOpTypeBool:",
        "            return finish(4);",
        "        case kOpTypeInt:",
        "        case kOpTypeFloat:",
        "            return finish((type.widthBits + 7U) / 8U);",
        "        case kOpTypeVector:",
        "        case kOpTypeMatrix: {",
        "            const uint64_t elementBytes = spirvTypeSizeBytes(",
        "                type.elementType, types, constants, arrayStrides, resolving);",
        "            return finish(checkedMul(elementBytes, type.elementCount));",
        "        }",
        "        case kOpTypeArray: {",
        "            const auto lengthIt = constants.find(type.lengthId);",
        "            if (lengthIt == constants.end())",
        "                return finish(0);",
        "            const auto strideIt = arrayStrides.find(typeId);",
        "            if (strideIt != arrayStrides.end())",
        "                return finish(checkedMul(strideIt->second, lengthIt->second));",
        "            const uint64_t elementBytes = spirvTypeSizeBytes(",
        "                type.elementType, types, constants, arrayStrides, resolving);",
        "            return finish(checkedMul(elementBytes, lengthIt->second));",
        "        }",
        "        case kOpTypeRuntimeArray:",
        "            return finish(0);",
        "        case kOpTypeStruct: {",
        "            uint64_t total = 0;",
        "            for (const uint32_t memberType : type.memberTypes)",
        "                total += spirvTypeSizeBytes(",
        "                    memberType, types, constants, arrayStrides, resolving);",
        "            return finish(total);",
        "        }",
        "        case kOpTypePointer:",
        "            return finish(spirvTypeSizeBytes(",
        "                type.elementType, types, constants, arrayStrides, resolving));",
        "        default:",
        "            return finish(0);",
        "        }",
        "    }",
        "",
        "    bool isImageSampleOp(uint16_t opCode) {",
        "        switch (opCode) {",
        "        case kOpImageSampleImplicitLod:",
        "        case kOpImageSampleExplicitLod:",
        "        case kOpImageSampleDrefImplicitLod:",
        "        case kOpImageSampleDrefExplicitLod:",
        "        case kOpImageSampleProjImplicitLod:",
        "        case kOpImageSampleProjExplicitLod:",
        "        case kOpImageSampleProjDrefImplicitLod:",
        "        case kOpImageSampleProjDrefExplicitLod:",
        "        case kOpImageGather:",
        "        case kOpImageDrefGather:",
        "            return true;",
        "        default:",
        "            return false;",
        "        }",
        "    }",
        "",
        "    void logMipmapsSpirvDump(",
        "            const std::string& shaderName,",
        "            const std::vector<uint8_t>& bytecode) {",
        "        if (shaderName != \"p_mipmaps\")",
        "            return;",
        "        static bool dumpedPerformanceMipmaps = false;",
        "        if (dumpedPerformanceMipmaps)",
        "            return;",
        "        dumpedPerformanceMipmaps = true;",
        "",
        "        const size_t chunkCount =",
        "            (bytecode.size() + kSpirvDumpBytesPerChunk - 1) / kSpirvDumpBytesPerChunk;",
        "        for (size_t chunk = 0; chunk < chunkCount; ++chunk) {",
        "            const size_t begin = chunk * kSpirvDumpBytesPerChunk;",
        "            const size_t end = std::min(begin + kSpirvDumpBytesPerChunk, bytecode.size());",
        "            std::ostringstream hex;",
        "            hex << std::hex << std::setfill('0');",
        "            for (size_t i = begin; i < end; ++i)",
        "                hex << std::setw(2) << static_cast<unsigned int>(bytecode.at(i));",
        "            std::cerr << \"lsfg-vk: zero-stage-shader-spirv-chunk\"",
        "                << \" shader=\" << shaderName",
        "                << \" chunk=\" << (chunk + 1)",
        "                << \" chunks=\" << chunkCount",
        "                << \" hex=\" << hex.str()",
        "                << std::endl;",
        "        }",
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
        "        size_t controlBarriers = 0;",
        "        size_t memoryBarriers = 0;",
        "        size_t imageReads = 0;",
        "        size_t imageWrites = 0;",
        "        size_t imageSamples = 0;",
        "        uint32_t localSizeX = 0;",
        "        uint32_t localSizeY = 0;",
        "        uint32_t localSizeZ = 0;",
        "        uint32_t localInvocationId = 0;",
        "        uint32_t workgroupId = 0;",
        "        uint64_t workgroupBytes = 0;",
        "        std::unordered_map<uint32_t, SpirvTypeInfo> types;",
        "        std::unordered_map<uint32_t, uint64_t> constants;",
        "        std::unordered_map<uint32_t, uint32_t> arrayStrides;",
        "        std::vector<uint32_t> workgroupTypeIds;",
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
        "                } else if (opCode == kOpTypeBool && instructionWordCount >= 2) {",
        "                    types[readSpirvWord(bytecode, cursor + 1)] = SpirvTypeInfo{.opCode = opCode};",
        "                } else if ((opCode == kOpTypeInt || opCode == kOpTypeFloat)",
        "                        && instructionWordCount >= 3) {",
        "                    SpirvTypeInfo type{.opCode = opCode};",
        "                    type.widthBits = readSpirvWord(bytecode, cursor + 2);",
        "                    types[readSpirvWord(bytecode, cursor + 1)] = std::move(type);",
        "                } else if ((opCode == kOpTypeVector || opCode == kOpTypeMatrix)",
        "                        && instructionWordCount >= 4) {",
        "                    SpirvTypeInfo type{.opCode = opCode};",
        "                    type.elementType = readSpirvWord(bytecode, cursor + 2);",
        "                    type.elementCount = readSpirvWord(bytecode, cursor + 3);",
        "                    types[readSpirvWord(bytecode, cursor + 1)] = std::move(type);",
        "                } else if (opCode == kOpTypeArray && instructionWordCount >= 4) {",
        "                    SpirvTypeInfo type{.opCode = opCode};",
        "                    type.elementType = readSpirvWord(bytecode, cursor + 2);",
        "                    type.lengthId = readSpirvWord(bytecode, cursor + 3);",
        "                    types[readSpirvWord(bytecode, cursor + 1)] = std::move(type);",
        "                } else if (opCode == kOpTypeRuntimeArray && instructionWordCount >= 3) {",
        "                    SpirvTypeInfo type{.opCode = opCode};",
        "                    type.elementType = readSpirvWord(bytecode, cursor + 2);",
        "                    types[readSpirvWord(bytecode, cursor + 1)] = std::move(type);",
        "                } else if (opCode == kOpTypeStruct && instructionWordCount >= 2) {",
        "                    SpirvTypeInfo type{.opCode = opCode};",
        "                    for (size_t i = 2; i < instructionWordCount; ++i)",
        "                        type.memberTypes.push_back(readSpirvWord(bytecode, cursor + i));",
        "                    types[readSpirvWord(bytecode, cursor + 1)] = std::move(type);",
        "                } else if (opCode == kOpTypePointer && instructionWordCount >= 4) {",
        "                    SpirvTypeInfo type{.opCode = opCode};",
        "                    type.storageClass = readSpirvWord(bytecode, cursor + 2);",
        "                    type.elementType = readSpirvWord(bytecode, cursor + 3);",
        "                    types[readSpirvWord(bytecode, cursor + 1)] = std::move(type);",
        "                } else if (opCode == kOpConstant && instructionWordCount >= 4) {",
        "                    constants[readSpirvWord(bytecode, cursor + 2)] =",
        "                        readSpirvWord(bytecode, cursor + 3);",
        "                } else if (opCode == kOpVariable && instructionWordCount >= 4) {",
        "                    const uint32_t storageClass = readSpirvWord(bytecode, cursor + 3);",
        "                    if (storageClass == kStorageClassWorkgroup) {",
        "                        ++workgroupVariables;",
        "                        workgroupTypeIds.push_back(readSpirvWord(bytecode, cursor + 1));",
        "                    }",
        "                } else if (opCode == kOpDecorate && instructionWordCount >= 4) {",
        "                    const uint32_t targetId = readSpirvWord(bytecode, cursor + 1);",
        "                    const uint32_t decoration = readSpirvWord(bytecode, cursor + 2);",
        "                    if (decoration == kDecorationArrayStride)",
        "                        arrayStrides[targetId] = readSpirvWord(bytecode, cursor + 3);",
        "                    else if (decoration == kDecorationBuiltIn) {",
        "                        const uint32_t builtIn = readSpirvWord(bytecode, cursor + 3);",
        "                        if (builtIn == kBuiltInLocalInvocationId)",
        "                            localInvocationId = targetId;",
        "                        else if (builtIn == kBuiltInWorkgroupId)",
        "                            workgroupId = targetId;",
        "                    }",
        "                }",
        "",
        "                if (opCode == kOpControlBarrier)",
        "                    ++controlBarriers;",
        "                else if (opCode == kOpMemoryBarrier)",
        "                    ++memoryBarriers;",
        "                else if (opCode == kOpImageRead || opCode == kOpImageFetch)",
        "                    ++imageReads;",
        "                else if (opCode == kOpImageWrite)",
        "                    ++imageWrites;",
        "                else if (isImageSampleOp(opCode))",
        "                    ++imageSamples;",
        "",
        "                cursor += instructionWordCount;",
        "            }",
        "        }",
        "",
        "        if (validSpirv) {",
        "            std::unordered_set<uint32_t> resolving;",
        "            for (const uint32_t typeId : workgroupTypeIds)",
        "                workgroupBytes += spirvTypeSizeBytes(",
        "                    typeId, types, constants, arrayStrides, resolving);",
        "        }",
        "",
        "        std::cerr << \"lsfg-vk: zero-stage-shader-profile\"",
        "            << \" shader=\" << shaderName",
        "            << \" spirv_bytes=\" << bytecode.size()",
        "            << \" spirv_words=\" << wordCount",
        "            << \" instruction_count=\" << instructionCount",
        "            << \" local_size=\" << localSizeX << 'x' << localSizeY << 'x' << localSizeZ",
        "            << \" workgroup_variables=\" << workgroupVariables",
        "            << \" workgroup_bytes=\" << workgroupBytes",
        "            << \" control_barriers=\" << controlBarriers",
        "            << \" memory_barriers=\" << memoryBarriers",
        "            << \" image_reads=\" << imageReads",
        "            << \" image_writes=\" << imageWrites",
        "            << \" image_samples=\" << imageSamples",
        "            << \" local_invocation_id=\" << localInvocationId",
        "            << \" workgroup_id=\" << workgroupId",
        "            << \" valid_spirv=\" << (validSpirv ? 1 : 0)",
        "            << std::endl;",
        "        logMipmapsSpirvDump(shaderName, bytecode);",
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
        "#include <cstdint>\n#include <cstddef>\n#include <algorithm>\n#include <cstring>\n#include <iomanip>\n#include <iostream>\n#include <limits>\n#include <sstream>\n#include <string>\n#include <unordered_map>\n#include <unordered_set>\n#include <utility>\n#include <vector>\n",
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
