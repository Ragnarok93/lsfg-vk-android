#!/usr/bin/env python3
from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PATCHER = ROOT / "scripts/apply-candidate-b6-pipeline-executable-profile.py"
SOURCE_PATHS = (
    "framegen/include/core/device.hpp",
    "framegen/src/core/device.cpp",
    "framegen/include/core/pipeline.hpp",
    "framegen/src/core/pipeline.cpp",
    "framegen/src/pool/shaderpool.cpp",
)


def read(root: Path, path: str) -> str:
    return (root / path).read_text(encoding="utf-8")


def require(text: str, *tokens: str) -> None:
    for token in tokens:
        assert token in text, f"missing required B6 token: {token}"


def snapshot(root: Path) -> dict[str, str]:
    return {path: read(root, path) for path in SOURCE_PATHS}


def main() -> None:
    assert PATCHER.is_file(), PATCHER

    with tempfile.TemporaryDirectory() as td:
        temp_root = Path(td)
        for relative in SOURCE_PATHS:
            source = ROOT / relative
            target = temp_root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)

        subprocess.run(
            [sys.executable, str(PATCHER), "--root", str(temp_root)],
            check=True,
        )
        first = snapshot(temp_root)
        subprocess.run(
            [sys.executable, str(PATCHER), "--root", str(temp_root)],
            check=True,
        )
        second = snapshot(temp_root)
        assert first == second, "B6 source transform is not idempotent"

        device_h = first["framegen/include/core/device.hpp"]
        device_cpp = first["framegen/src/core/device.cpp"]
        pipeline_h = first["framegen/include/core/pipeline.hpp"]
        pipeline_cpp = first["framegen/src/core/pipeline.cpp"]
        shaderpool_cpp = first["framegen/src/pool/shaderpool.cpp"]

        # Optional capability probing/enablement must leave unsupported devices
        # on the pre-B6 creation path.
        require(device_h,
            "supportsPipelineExecutableProperties()",
            "pipelineExecutablePropertiesSupported{false}")
        require(device_cpp,
            "VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME",
            "VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR",
            "pipelineExecutableInfo",
            "pipelineExecutablePropertiesSupported",
            "enabledExtensions.push_back(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME)",
            "pipeline-exec-capability supported=")

        # ShaderPool owns the canonical extracted name; preserve it through the
        # pipeline constructor and capture only the p_mipmaps pipeline.
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
            "pipeline-exec-ir shader=p_mipmaps",
            "pipeline-exec-ir-line shader=p_mipmaps",
            "captureExecutableInfo ?",
            ": 0")

    build = read(ROOT, "scripts/build/android.sh")
    invocation = (
        'python3 "${REPO_ROOT}/scripts/apply-candidate-b6-pipeline-executable-profile.py" '
        '--root "${REPO_ROOT}"'
    )
    require(build, invocation)
    profile_start = build.index('if [[ "${LSFGVK_ZERO_STAGE_PROFILE:-0}" == "1" ]]')
    profile_end = build.index("\nfi\n", profile_start)
    invocation_index = build.index(invocation)
    assert profile_start < invocation_index < profile_end, (
        "B6 profiler must remain profiling-build-only"
    )

    print("Candidate B6 pipeline executable diagnostics contract satisfied")


if __name__ == "__main__":
    main()
