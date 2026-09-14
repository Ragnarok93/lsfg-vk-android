#!/usr/bin/env python3
"""Candidate B6: capture Vulkan pipeline-executable diagnostics for p_mipmaps.

This profiling-only source transform enables VK_KHR_pipeline_executable_properties
when the framegen device exposes it, captures statistics/internal representations
only for the p_mipmaps compute pipeline, and logs the returned data. Unsupported
devices keep normal pipeline creation and never fail LSFG because of profiling.
"""
from __future__ import annotations

import argparse
from pathlib import Path

DEVICE_HPP = Path("framegen/include/core/device.hpp")
DEVICE_CPP = Path("framegen/src/core/device.cpp")
PIPELINE_HPP = Path("framegen/include/core/pipeline.hpp")
PIPELINE_CPP = Path("framegen/src/core/pipeline.cpp")
SHADERPOOL_CPP = Path("framegen/src/pool/shaderpool.cpp")
MARKER = "pipeline-exec-profile shader=p_mipmaps"


def replace_exact(text: str, old: str, new: str, label: str) -> str:
    if new in text:
        return text
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected one anchor, found {count}")
    return text.replace(old, new, 1)


def patch_device_hpp(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    text = replace_exact(
        text,
        "        [[nodiscard]] bool supportsNullDescriptor() const { return this->nullDescriptorSupported; }\n",
        "        [[nodiscard]] bool supportsNullDescriptor() const { return this->nullDescriptorSupported; }\n"
        "        [[nodiscard]] bool supportsPipelineExecutableProperties() const {\n"
        "            return this->pipelineExecutablePropertiesSupported;\n"
        "        }\n",
        "device.hpp public capability",
    )
    text = replace_exact(
        text,
        "        bool nullDescriptorSupported{false};\n",
        "        bool nullDescriptorSupported{false};\n"
        "        bool pipelineExecutablePropertiesSupported{false};\n",
        "device.hpp private capability",
    )
    path.write_text(text, encoding="utf-8")


def patch_device_cpp(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    text = replace_exact(
        text,
        "    const bool hasTimelineExt = hasExtension(availableExtensions,\n"
        "        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);\n",
        "    const bool hasTimelineExt = hasExtension(availableExtensions,\n"
        "        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);\n"
        "    const bool hasPipelineExecutablePropertiesExt = hasExtension(availableExtensions,\n"
        "        VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);\n",
        "device.cpp extension probe",
    )
    text = replace_exact(
        text,
        "    void* featureProbeHead = nullptr;\n",
        "    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR pipelineExecutableProbe{\n"
        "        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR,\n"
        "    };\n"
        "\n"
        "    void* featureProbeHead = nullptr;\n"
        "    if (hasPipelineExecutablePropertiesExt) {\n"
        "        pipelineExecutableProbe.pNext = featureProbeHead;\n"
        "        featureProbeHead = &pipelineExecutableProbe;\n"
        "    }\n",
        "device.cpp feature probe chain",
    )
    text = replace_exact(
        text,
        "    if (!api12 && caps.shaderFloat16)\n"
        "        enabledExtensions.push_back(VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);\n",
        "    if (!api12 && caps.shaderFloat16)\n"
        "        enabledExtensions.push_back(VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);\n"
        "    const bool enablePipelineExecutableProperties = hasPipelineExecutablePropertiesExt\n"
        "        && pipelineExecutableProbe.pipelineExecutableInfo == VK_TRUE;\n"
        "    if (enablePipelineExecutableProperties)\n"
        "        enabledExtensions.push_back(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);\n",
        "device.cpp optional extension enable",
    )
    text = replace_exact(
        text,
        "    void* enableHead = enableNullDescriptor ? &robustnessEnable : nullptr;\n",
        "    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR pipelineExecutableEnable{\n"
        "        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR,\n"
        "        .pipelineExecutableInfo = enablePipelineExecutableProperties ? VK_TRUE : VK_FALSE,\n"
        "    };\n"
        "\n"
        "    void* enableHead = enableNullDescriptor ? &robustnessEnable : nullptr;\n"
        "    if (enablePipelineExecutableProperties) {\n"
        "        pipelineExecutableEnable.pNext = enableHead;\n"
        "        enableHead = &pipelineExecutableEnable;\n"
        "    }\n",
        "device.cpp feature enable chain",
    )
    text = replace_exact(
        text,
        "    this->nullDescriptorSupported = enableNullDescriptor;\n",
        "    this->nullDescriptorSupported = enableNullDescriptor;\n"
        "    this->pipelineExecutablePropertiesSupported = enablePipelineExecutableProperties;\n"
        "    std::cerr << \"lsfg-vk: pipeline-exec-capability supported=\"\n"
        "              << (enablePipelineExecutableProperties ? 1 : 0) << '\\n';\n",
        "device.cpp capability state",
    )
    path.write_text(text, encoding="utf-8")


def patch_pipeline_hpp(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    text = replace_exact(text, "#include <memory>\n", "#include <memory>\n#include <string>\n", "pipeline.hpp string include")
    text = replace_exact(
        text,
        "        Pipeline(const Core::Device& device, const ShaderModule& shader);\n",
        "        Pipeline(const Core::Device& device, const ShaderModule& shader,\n"
        "            const std::string& shaderName);\n",
        "pipeline.hpp constructor",
    )
    path.write_text(text, encoding="utf-8")


PIPELINE_HELPER = r'''namespace {

const char* statisticFormatName(VkPipelineExecutableStatisticFormatKHR format) {
    switch (format) {
    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR: return "bool32";
    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR: return "int64";
    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR: return "uint64";
    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR: return "float64";
    default: return "unknown";
    }
}

std::string statisticValue(const VkPipelineExecutableStatisticKHR& statistic) {
    std::ostringstream out;
    switch (statistic.format) {
    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR:
        out << (statistic.value.b32 ? 1 : 0); break;
    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR:
        out << statistic.value.i64; break;
    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR:
        out << statistic.value.u64; break;
    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR:
        out << statistic.value.f64; break;
    default:
        out << "unknown"; break;
    }
    return out.str();
}

std::string escapePipelineText(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char c : text) {
        if (c == '\\') escaped += "\\\\";
        else if (c == '"') escaped += "\\\"";
        else if (c == '\t') escaped += "\\t";
        else if (static_cast<unsigned char>(c) < 0x20U && c != '\n' && c != '\r') escaped += '?';
        else escaped += c;
    }
    return escaped;
}

void logPipelineExecutableProfile(VkDevice device, VkPipeline pipeline,
        const std::string& shaderName) {
    if (shaderName != "p_mipmaps") return;
    if (vkGetPipelineExecutablePropertiesKHR == nullptr
            || vkGetPipelineExecutableStatisticsKHR == nullptr
            || vkGetPipelineExecutableInternalRepresentationsKHR == nullptr) {
        std::cerr << "lsfg-vk: pipeline-exec-profile shader=p_mipmaps supported=0 reason=entrypoints\n";
        return;
    }

    const VkPipelineInfoKHR pipelineInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR,
        .pipeline = pipeline,
    };
    uint32_t executableCount = 0;
    VkResult result = vkGetPipelineExecutablePropertiesKHR(
        device, &pipelineInfo, &executableCount, nullptr);
    if (result != VK_SUCCESS) {
        std::cerr << "lsfg-vk: pipeline-exec-profile shader=p_mipmaps supported=1 result="
                  << result << " executable_count=0\n";
        return;
    }

    std::vector<VkPipelineExecutablePropertiesKHR> properties(executableCount);
    for (auto& property : properties)
        property.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR;
    if (executableCount > 0) {
        result = vkGetPipelineExecutablePropertiesKHR(
            device, &pipelineInfo, &executableCount, properties.data());
    }
    std::cerr << "lsfg-vk: pipeline-exec-profile shader=p_mipmaps supported=1 result="
              << result << " executable_count=" << executableCount << '\n';
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) return;

    for (uint32_t executableIndex = 0; executableIndex < executableCount; ++executableIndex) {
        const auto& property = properties[executableIndex];
        std::cerr << "lsfg-vk: pipeline-exec-property shader=p_mipmaps executable="
                  << executableIndex << " stages=0x" << std::hex << property.stages << std::dec
                  << " subgroup_size=" << property.subgroupSize
                  << " name=\"" << escapePipelineText(property.name) << "\""
                  << " description=\"" << escapePipelineText(property.description) << "\"\n";

        const VkPipelineExecutableInfoKHR executableInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR,
            .pipeline = pipeline,
            .executableIndex = executableIndex,
        };

        uint32_t statisticCount = 0;
        result = vkGetPipelineExecutableStatisticsKHR(
            device, &executableInfo, &statisticCount, nullptr);
        if (result == VK_SUCCESS && statisticCount > 0) {
            std::vector<VkPipelineExecutableStatisticKHR> statistics(statisticCount);
            for (auto& statistic : statistics)
                statistic.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR;
            result = vkGetPipelineExecutableStatisticsKHR(
                device, &executableInfo, &statisticCount, statistics.data());
            if (result == VK_SUCCESS || result == VK_INCOMPLETE) {
                for (uint32_t i = 0; i < statisticCount; ++i) {
                    const auto& statistic = statistics[i];
                    std::cerr << "lsfg-vk: pipeline-exec-stat shader=p_mipmaps executable="
                              << executableIndex << " stat=" << i
                              << " format=" << statisticFormatName(statistic.format)
                              << " value=" << statisticValue(statistic)
                              << " name=\"" << escapePipelineText(statistic.name) << "\""
                              << " description=\"" << escapePipelineText(statistic.description)
                              << "\"\n";
                }
            }
        }

        uint32_t representationCount = 0;
        result = vkGetPipelineExecutableInternalRepresentationsKHR(
            device, &executableInfo, &representationCount, nullptr);
        if (result != VK_SUCCESS || representationCount == 0) continue;

        std::vector<VkPipelineExecutableInternalRepresentationKHR> representations(representationCount);
        for (auto& representation : representations)
            representation.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR;
        result = vkGetPipelineExecutableInternalRepresentationsKHR(
            device, &executableInfo, &representationCount, representations.data());
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) continue;

        std::vector<std::vector<uint8_t>> storage(representationCount);
        for (uint32_t i = 0; i < representationCount; ++i) {
            storage[i].resize(representations[i].dataSize);
            representations[i].pData = storage[i].empty() ? nullptr : storage[i].data();
        }
        result = vkGetPipelineExecutableInternalRepresentationsKHR(
            device, &executableInfo, &representationCount, representations.data());
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) continue;

        for (uint32_t representationIndex = 0;
                representationIndex < representationCount; ++representationIndex) {
            const auto& representation = representations[representationIndex];
            std::cerr << "lsfg-vk: pipeline-exec-ir shader=p_mipmaps executable="
                      << executableIndex << " ir=" << representationIndex
                      << " text=" << (representation.isText ? 1 : 0)
                      << " bytes=" << representation.dataSize
                      << " name=\"" << escapePipelineText(representation.name) << "\""
                      << " description=\"" << escapePipelineText(representation.description)
                      << "\"\n";
            if (!representation.isText || representation.pData == nullptr
                    || representation.dataSize == 0) continue;

            std::string text(static_cast<const char*>(representation.pData), representation.dataSize);
            while (!text.empty() && text.back() == '\0') text.pop_back();
            size_t lineStart = 0;
            uint32_t lineIndex = 0;
            while (lineStart <= text.size()) {
                const size_t lineEnd = text.find('\n', lineStart);
                const size_t end = lineEnd == std::string::npos ? text.size() : lineEnd;
                const std::string escaped = escapePipelineText(
                    std::string_view(text).substr(lineStart, end - lineStart));
                constexpr size_t chunkSize = 1800;
                const size_t chunkCount = std::max<size_t>(1, (escaped.size() + chunkSize - 1) / chunkSize);
                for (size_t chunk = 0; chunk < chunkCount; ++chunk) {
                    std::cerr << "lsfg-vk: pipeline-exec-ir-line shader=p_mipmaps executable="
                              << executableIndex << " ir=" << representationIndex
                              << " line=" << lineIndex << " chunk=" << chunk
                              << " chunks=" << chunkCount << " text=\""
                              << escaped.substr(chunk * chunkSize, chunkSize) << "\"\n";
                }
                ++lineIndex;
                if (lineEnd == std::string::npos) break;
                lineStart = lineEnd + 1;
            }
        }
    }
}

} // namespace

'''


def patch_pipeline_cpp(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if MARKER in text:
        return
    text = replace_exact(
        text,
        "#include <memory>\n",
        "#include <algorithm>\n#include <cstdint>\n#include <iostream>\n#include <memory>\n"
        "#include <sstream>\n#include <string>\n#include <string_view>\n#include <vector>\n",
        "pipeline.cpp includes",
    )
    text = replace_exact(text, "using namespace LSFG::Core;\n", PIPELINE_HELPER + "using namespace LSFG::Core;\n", "pipeline.cpp helper")
    text = replace_exact(
        text,
        "Pipeline::Pipeline(const Core::Device& device, const ShaderModule& shader) {\n",
        "Pipeline::Pipeline(const Core::Device& device, const ShaderModule& shader,\n"
        "        const std::string& shaderName) {\n",
        "pipeline.cpp constructor",
    )
    text = replace_exact(
        text,
        "    const VkComputePipelineCreateInfo pipelineDesc{\n"
        "        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,\n",
        "    const bool captureExecutableInfo = shaderName == \"p_mipmaps\"\n"
        "        && device.supportsPipelineExecutableProperties();\n"
        "    const VkPipelineCreateFlags executableCaptureFlags = captureExecutableInfo ?\n"
        "        (VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR\n"
        "            | VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR) : 0;\n"
        "    const VkComputePipelineCreateInfo pipelineDesc{\n"
        "        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,\n"
        "        .flags = executableCaptureFlags,\n",
        "pipeline.cpp capture flags",
    )
    text = replace_exact(
        text,
        "    if (res != VK_SUCCESS || !pipelineHandle)\n"
        "        throw LSFG::vulkan_error(res, \"Failed to create compute pipeline\");\n\n"
        "    // store layout and pipeline in shared ptr\n",
        "    if (res != VK_SUCCESS || !pipelineHandle)\n"
        "        throw LSFG::vulkan_error(res, \"Failed to create compute pipeline\");\n"
        "    if (captureExecutableInfo)\n"
        "        logPipelineExecutableProfile(device.handle(), pipelineHandle, shaderName);\n\n"
        "    // store layout and pipeline in shared ptr\n",
        "pipeline.cpp query call",
    )
    path.write_text(text, encoding="utf-8")


def patch_shaderpool_cpp(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    text = replace_exact(
        text,
        "    Core::Pipeline pipeline(device, shader);\n",
        "    Core::Pipeline pipeline(device, shader, name);\n",
        "shaderpool.cpp shader name propagation",
    )
    path.write_text(text, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    root = args.root.resolve()
    patch_device_hpp(root / DEVICE_HPP)
    patch_device_cpp(root / DEVICE_CPP)
    patch_pipeline_hpp(root / PIPELINE_HPP)
    patch_pipeline_cpp(root / PIPELINE_CPP)
    patch_shaderpool_cpp(root / SHADERPOOL_CPP)


if __name__ == "__main__":
    main()
