#!/usr/bin/env python3
from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PATCHER = ROOT / "scripts/apply-candidate-b8-mipmaps-matrix.py"
SOURCE_PATHS = (
    "src/extract/trans.cpp",
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
    assert PATCHER.is_file(), PATCHER

    # B8 composes after the existing B6 profiler and B4 production rewrite.
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
        assert first == second, "B8 source transform is not idempotent"

        trans = first["src/extract/trans.cpp"]
        pipeline_h = first["framegen/include/core/pipeline.hpp"]
        pipeline_cpp = first["framegen/src/core/pipeline.cpp"]
        shaderpool = first["framegen/src/pool/shaderpool.cpp"]

        # One device run must compile a useful candidate matrix from one exact
        # p_mipmaps input. Only semantics-preserving variants may be selected.
        require(trans,
            "candidate-b8-mipmaps-matrix",
            "baseline",
            "b7-control",
            "pow-equivalent",
            "transfer-bypass-upper-bound",
            "no-u0-writes-upper-bound",
            "local16-occupancy-probe",
            "compile_only=1")

        # Pipeline executable capture must return compact machine statistics so
        # ShaderPool can compare candidates without parsing its own log output.
        require(pipeline_h,
            "PipelineExecutableSummary",
            "maxWaves",
            "instructionCount",
            "nopCount",
            "registerCount",
            "ssStallCycles",
            "syStallCycles",
            "stpCount",
            "ldpCount")
        require(pipeline_cpp,
            "pipeline-exec-summary",
            "Estimated cycles stalled on SS",
            "Estimated cycles stalled on SY",
            "STP Count",
            "LDP Count")

        # Selection is deterministic and compile-only probes can never become
        # the runtime p_mipmaps pipeline.
        require(shaderpool,
            "mipmaps-variant-score",
            "mipmaps-variant-selected",
            "compileOnly",
            "semanticsPreserving",
            "strictlyBetterMipmapsVariant")

    build = read(ROOT, "scripts/build/android.sh")
    invocation = (
        'python3 "${REPO_ROOT}/scripts/apply-candidate-b8-mipmaps-matrix.py" '
        '--root "${REPO_ROOT}"'
    )
    require(build, invocation)
    profile_start = build.index('if [[ "${LSFGVK_ZERO_STAGE_PROFILE:-0}" == "1" ]]')
    profile_end = build.index("\nfi\n", profile_start)
    invocation_index = build.index(invocation)
    assert profile_start < invocation_index < profile_end, (
        "B8 candidate matrix must remain profiling-build-only"
    )

    print("Candidate B8 mipmaps matrix contract satisfied")


if __name__ == "__main__":
    main()
