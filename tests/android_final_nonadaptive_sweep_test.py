#!/usr/bin/env python3
from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
B6 = ROOT / "scripts/apply-candidate-b6-pipeline-executable-profile.py"
SWEEP = ROOT / "scripts/apply-final-nonadaptive-sweep.py"
SOURCE_PATHS = (
    "framegen/include/core/device.hpp",
    "framegen/src/core/device.cpp",
    "framegen/include/core/pipeline.hpp",
    "framegen/src/core/pipeline.cpp",
    "framegen/include/pool/shaderpool.hpp",
    "framegen/src/pool/shaderpool.cpp",
    "framegen/v3.1p_src/context.cpp",
)


def read(root: Path, relative: str) -> str:
    return (root / relative).read_text(encoding="utf-8")


def require(text: str, *tokens: str) -> None:
    for token in tokens:
        assert token in text, f"missing final-sweep token: {token}"


def main() -> None:
    assert B6.is_file(), B6
    assert SWEEP.is_file(), SWEEP

    with tempfile.TemporaryDirectory() as td:
        temp = Path(td)
        for relative in SOURCE_PATHS:
            src = ROOT / relative
            dst = temp / relative
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, dst)

        subprocess.run([sys.executable, str(B6), "--root", str(temp)], check=True)
        subprocess.run([sys.executable, str(SWEEP), "--root", str(temp)], check=True)
        first = {p: read(temp, p) for p in SOURCE_PATHS}
        subprocess.run([sys.executable, str(SWEEP), "--root", str(temp)], check=True)
        second = {p: read(temp, p) for p in SOURCE_PATHS}
        assert first == second, "final nonadaptive sweep transform is not idempotent"

        pipeline = first["framegen/src/core/pipeline.cpp"]
        pool_hpp = first["framegen/include/pool/shaderpool.hpp"]
        pool_cpp = first["framegen/src/pool/shaderpool.cpp"]
        context = first["framegen/v3.1p_src/context.cpp"]

        require(
            pipeline,
            'shaderName.rfind("p_mipmaps", 0) == 0',
            "pipeline-exec-stat shader=p_mipmaps",
            "pipeline-exec-ir-line shader=p_mipmaps",
        )
        require(pool_hpp, "runNextAdrenoSweepProbe", "adrenoSweepProbeIndex")
        require(
            pool_cpp,
            "final-nonadaptive-sweep",
            "mipmaps-pow-equivalent",
            "mipmaps-transfer-bypass-upper-bound",
            "mipmaps-no-u0-writes-upper-bound",
            "mipmaps-local16-occupancy-probe",
            "mipmaps-local8-occupancy-probe",
            "beta4-baseline",
            "beta4-half-samples-upper-bound",
            "beta4-single-sample-upper-bound",
            "beta4-no-output-writes-upper-bound",
            "beta4-local16-occupancy-probe",
            "sweep-probe-begin",
            "sweep-probe-end",
            "compile_only=1",
            "kSweepOpImageSampleExplicitLod",
            "kSweepOpCopyObject",
        )
        require(
            context,
            "kAdrenoSweepWarmupFrames",
            "kAdrenoSweepIntervalFrames",
            "runNextAdrenoSweepProbe",
        )
        assert "flowScale" not in pool_cpp
        assert "adaptive_scheduler" not in pool_cpp
        assert "LSFGVK_B8_MIPMAPS_VARIANT" not in pool_cpp

    build = read(ROOT, "scripts/build/android.sh")
    require(
        build,
        'LSFGVK_FINAL_NONADAPTIVE_SWEEP',
        'apply-candidate-b6-pipeline-executable-profile.py',
        'apply-final-nonadaptive-sweep.py',
    )
    gate = build.index('LSFGVK_FINAL_NONADAPTIVE_SWEEP')
    b6 = build.index('apply-candidate-b6-pipeline-executable-profile.py', gate)
    sweep = build.index('apply-final-nonadaptive-sweep.py', gate)
    assert gate < b6 < sweep, "final sweep must apply B6 before deferred probe transform"

    print("Final nonadaptive sweep contract satisfied")


if __name__ == "__main__":
    main()
