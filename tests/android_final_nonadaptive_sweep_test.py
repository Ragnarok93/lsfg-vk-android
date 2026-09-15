#!/usr/bin/env python3
from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
B6 = ROOT / "scripts/apply-candidate-b6-pipeline-executable-profile.py"
B8 = ROOT / "scripts/apply-candidate-b8-mipmaps-matrix.py"
B8_CONSTANTS = ROOT / "scripts/apply-candidate-b8-local-spirv-constants.py"
SWEEP = ROOT / "scripts/apply-final-nonadaptive-sweep-v2.py"
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
    for patcher in (B6, B8, B8_CONSTANTS, SWEEP):
        assert patcher.is_file(), patcher

    with tempfile.TemporaryDirectory() as td:
        temp = Path(td)
        for relative in SOURCE_PATHS:
            src = ROOT / relative
            dst = temp / relative
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, dst)

        for patcher in (B6, B8, B8_CONSTANTS):
            subprocess.run([sys.executable, str(patcher), "--root", str(temp)], check=True)

        context_path = temp / "framegen/v3.1p_src/context.cpp"
        context_text = context_path.read_text(encoding="utf-8")
        old_data_anchor = "    auto& data = this->data.at(this->frameIdx % 8);\n\n"
        assert old_data_anchor in context_text
        context_text = context_text.replace(
            old_data_anchor,
            "    auto& data = this->data.at(this->frameIdx % 8);\n"
            "    // simulated-prior-profile-transform\n\n",
            1,
        )
        context_path.write_text(context_text, encoding="utf-8")

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
            'shaderName.rfind("p_mipmaps@sweep-", 0) == 0',
            "pipeline-exec-stat shader=p_mipmaps",
            "pipeline-exec-ir-line shader=p_mipmaps",
        )
        require(pool_hpp, "runNextAdrenoSweepProbe", "adrenoSweepProbeIndex")
        require(
            pool_cpp,
            "final-nonadaptive-sweep",
            "mipmaps-baseline",
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
            "beta4-local8-occupancy-probe",
            "sweep-probe-begin",
            "sweep-probe-end",
            "compile_only=1",
            "kSweepOpImageSampleExplicitLod",
            "kSweepOpCopyObject",
            "sweep-probe-descriptor-layout",
            "sweep-independent-mipmaps-probes",
            "if (index == 1U) return b8PowEquivalent(baseline, out);",
            "if (index == 2U) return b8TransferBypass(baseline, out);",
            "if (index == 3U) return b8NoU0Writes(baseline, out);",
            "if (index == 4U) return b8Local16(baseline, out);",
            "if (index == 5U) return b8Local8(baseline, out);",
            "{7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}",
            "{6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}",
        )
        assert "if (variants.size() != 6U) return false;" not in pool_cpp
        require(
            context,
            "kAdrenoSweepWarmupFrames",
            "kAdrenoSweepIntervalFrames",
            "runNextAdrenoSweepProbe",
            "simulated-prior-profile-transform",
        )
        assert context.index("simulated-prior-profile-transform") < context.index("runNextAdrenoSweepProbe")
        assert context.index("runNextAdrenoSweepProbe") < context.index("if (data.shouldWait)")
        assert "flowScale" not in pool_cpp
        assert "adaptive_scheduler" not in pool_cpp
        assert "LSFGVK_B8_MIPMAPS_VARIANT" not in pool_cpp
        assert 'if (name != "p_mipmaps"' not in pool_cpp

    build = read(ROOT, "scripts/build/android.sh")
    require(
        build,
        'LSFGVK_FINAL_NONADAPTIVE_SWEEP',
        'apply-candidate-b6-pipeline-executable-profile.py',
        'apply-candidate-b8-mipmaps-matrix.py',
        'apply-candidate-b8-local-spirv-constants.py',
        'apply-final-nonadaptive-sweep-v2.py',
    )
    gate = build.index('if [[ "${LSFGVK_FINAL_NONADAPTIVE_SWEEP:-0}" == "1" ]]')
    b6 = build.index('apply-candidate-b6-pipeline-executable-profile.py', gate)
    b8 = build.index('apply-candidate-b8-mipmaps-matrix.py', gate)
    constants = build.index('apply-candidate-b8-local-spirv-constants.py', gate)
    sweep = build.index('apply-final-nonadaptive-sweep-v2.py', gate)
    assert gate < b6 < b8 < constants < sweep, (
        "final sweep must compose B6/B8 helpers before deferred probe transform"
    )

    print("Final nonadaptive sweep contract satisfied")


if __name__ == "__main__":
    main()
