#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidCleanBenchmarkModeContractTest(unittest.TestCase):
    def test_runtime_only_mode_keeps_adaptive_lifecycle_without_heavy_profilers(self) -> None:
        build_script = (ROOT / "scripts/build/android.sh").read_text(encoding="utf-8")
        bundle_script = (ROOT / "scripts/apply-adreno-evidence-profile.py").read_text(
            encoding="utf-8"
        )

        self.assertIn("LSFGVK_ADAPTIVE_RUNTIME", build_script)
        runtime_start = build_script.index('if [[ "${LSFGVK_ADAPTIVE_RUNTIME:-0}" == "1" ]]')
        runtime_end = build_script.index("\nfi", runtime_start)
        runtime_block = build_script[runtime_start:runtime_end]
        self.assertIn("apply-adreno-evidence-profile.py", runtime_block)
        self.assertIn("--runtime-only", runtime_block)
        for forbidden in (
            "apply-zero-stage-profile.py",
            "apply-mipmaps-shader-profile.py",
            "apply-candidate-b6-pipeline-executable-profile.py",
            "apply-candidate-b8-mipmaps-matrix.py",
            "apply-candidate-b8-local-spirv-constants.py",
            "apply-final-nonadaptive-sweep-v2.py",
        ):
            self.assertNotIn(forbidden, runtime_block)

        self.assertIn('"--runtime-only"', bundle_script)
        self.assertIn('action="store_true"', bundle_script)
        self.assertIn("def apply_runtime(root: Path) -> None:", bundle_script)
        self.assertIn("def apply_profiling(root: Path) -> None:", bundle_script)

        runtime_fn_start = bundle_script.index("def apply_runtime(root: Path) -> None:")
        profiling_fn_start = bundle_script.index("def apply_profiling(root: Path) -> None:")
        runtime_fn = bundle_script[runtime_fn_start:profiling_fn_start]
        self.assertIn("apply_syncfd_handoff(root)", runtime_fn)
        self.assertIn("apply_async_zero_history(root)", runtime_fn)
        self.assertIn("apply_deferred_zero_safe_reprime(root)", runtime_fn)
        self.assertIn("apply_deferred_zero_exit_persistence(root)", runtime_fn)
        self.assertNotIn("patch_timestamp_query_pool(", runtime_fn)
        self.assertNotIn("patch_framegen_header(", runtime_fn)
        self.assertNotIn("patch_framegen_source(", runtime_fn)

        profiling_fn = bundle_script[profiling_fn_start:]
        self.assertIn("patch_timestamp_query_pool(", profiling_fn)
        self.assertIn("patch_framegen_header(", profiling_fn)
        self.assertIn("patch_framegen_source(", profiling_fn)


if __name__ == "__main__":
    unittest.main()
