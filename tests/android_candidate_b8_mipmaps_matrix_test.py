#!/usr/bin/env python3
from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
B6_PATCHER = ROOT / "scripts/apply-candidate-b6-pipeline-executable-profile.py"
PATCHER = ROOT / "scripts/apply-candidate-b8-mipmaps-matrix.py"
CONSTANTS_PATCHER = ROOT / "scripts/apply-candidate-b8-local-spirv-constants.py"
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
        assert token in text, f"missing required B8 token: {token}"


def snapshot(root: Path) -> dict[str, str]:
    return {path: read(root, path) for path in SOURCE_PATHS}


def main() -> None:
    assert B6_PATCHER.is_file(), B6_PATCHER
    assert PATCHER.is_file(), PATCHER
    assert CONSTANTS_PATCHER.is_file(), CONSTANTS_PATCHER

    with tempfile.TemporaryDirectory() as td:
        temp_root = Path(td)
        for relative in SOURCE_PATHS:
            source = ROOT / relative
            target = temp_root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)

        subprocess.run([sys.executable, str(B6_PATCHER), "--root", str(temp_root)], check=True)
        subprocess.run([sys.executable, str(PATCHER), "--root", str(temp_root)], check=True)
        subprocess.run([sys.executable, str(CONSTANTS_PATCHER), "--root", str(temp_root)], check=True)
        first = snapshot(temp_root)
        subprocess.run([sys.executable, str(PATCHER), "--root", str(temp_root)], check=True)
        subprocess.run([sys.executable, str(CONSTANTS_PATCHER), "--root", str(temp_root)], check=True)
        second = snapshot(temp_root)
        assert first == second, "B8 source transform is not idempotent"

        pipeline_cpp = first["framegen/src/core/pipeline.cpp"]
        shaderpool = first["framegen/src/pool/shaderpool.cpp"]

        require(pipeline_cpp,
            'shaderName.rfind("p_mipmaps", 0) == 0',
            "pipeline-exec-stat shader=p_mipmaps",
            "pipeline-exec-ir-line shader=p_mipmaps")

        require(shaderpool,
            "candidate-b8-mipmaps-matrix",
            '"baseline"',
            '"pow-equivalent"',
            '"transfer-bypass-upper-bound"',
            '"no-u0-writes-upper-bound"',
            '"local16-occupancy-probe"',
            '"local8-occupancy-probe"',
            "mipmaps-variant-begin",
            "mipmaps-variant-end",
            "mipmaps-variant-selected",
            "LSFGVK_B8_MIPMAPS_VARIANT",
            "compileOnly",
            "semanticsPreserving",
            "kB8OpExtInst",
            "kB8OpExecutionMode",
            "kB8OpCopyObject",
            "kB8OpImageWrite",
            "kB8OpFMul",
            "kB8OpControlBarrier",
            "kB8ExecutionModeLocalSize")
        assert "thirdparty/spirv.hpp" not in shaderpool
        assert "spv::" not in shaderpool

    build = read(ROOT, "scripts/build/android.sh")
    invocation = (
        'python3 "${REPO_ROOT}/scripts/apply-candidate-b8-mipmaps-matrix.py" '
        '--root "${REPO_ROOT}"'
    )
    constants_invocation = (
        'python3 "${REPO_ROOT}/scripts/apply-candidate-b8-local-spirv-constants.py" '
        '--root "${REPO_ROOT}"'
    )
    require(build, invocation, constants_invocation)
    profile_start = build.index('if [[ "${LSFGVK_ZERO_STAGE_PROFILE:-0}" == "1" ]]')
    profile_end = build.index("\nfi\n", profile_start)
    invocation_index = build.index(invocation)
    constants_index = build.index(constants_invocation)
    assert profile_start < invocation_index < constants_index < profile_end, (
        "B8 candidate matrix and constants fix must remain profiling-build-only and ordered"
    )

    print("Candidate B8 mipmaps matrix contract satisfied")


if __name__ == "__main__":
    main()
