#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def require(text: str, *tokens: str) -> None:
    for token in tokens:
        assert token in text, f"missing required B6 token: {token}"


def main() -> None:
    device_h = read("framegen/include/core/device.hpp")
    device_cpp = read("framegen/src/core/device.cpp")
    pipeline_h = read("framegen/include/core/pipeline.hpp")
    pipeline_cpp = read("framegen/src/core/pipeline.cpp")
    shaderpool_cpp = read("framegen/src/pool/shaderpool.cpp")

    # The diagnostic capability is optional and observable from Pipeline.
    require(device_h,
        "supportsPipelineExecutableProperties()",
        "pipelineExecutablePropertiesSupported{false}")
    require(device_cpp,
        "VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME",
        "VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR",
        "pipelineExecutableInfo",
        "pipelineExecutablePropertiesSupported",
        "enabledExtensions.push_back(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME)")

    # ShaderPool already owns the canonical extracted shader name. Preserve it
    # through pipeline construction so diagnostics are strictly p_mipmaps-only.
    require(pipeline_h, "const std::string& shaderName")
    require(shaderpool_cpp, "Core::Pipeline pipeline(device, shader, name);")
    require(pipeline_cpp,
        'shaderName == "p_mipmaps"',
        "VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR",
        "VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR",
        "vkGetPipelineExecutablePropertiesKHR",
        "vkGetPipelineExecutableStatisticsKHR",
        "vkGetPipelineExecutableInternalRepresentationsKHR",
        "pipeline-exec-profile shader=p_mipmaps",
        "pipeline-exec-stat shader=p_mipmaps",
        "pipeline-exec-ir shader=p_mipmaps")

    # Unsupported devices must retain normal compute-pipeline creation flags.
    require(pipeline_cpp, "captureExecutableInfo ?", ": 0")

    print("Candidate B6 pipeline executable diagnostics contract satisfied")


if __name__ == "__main__":
    main()
