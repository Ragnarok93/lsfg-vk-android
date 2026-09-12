#!/usr/bin/env python3
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidAdrenoEvidenceBundleContractTest(unittest.TestCase):
    def test_profile_build_bundles_sync_transport_generated_and_failure_diagnostics(self) -> None:
        zero_stage = ROOT / "scripts/apply-zero-stage-profile.py"
        shader_profile = ROOT / "scripts/apply-mipmaps-shader-profile.py"
        evidence_profile = ROOT / "scripts/apply-adreno-evidence-profile.py"
        build_script = ROOT / "scripts/build/android.sh"

        self.assertTrue(zero_stage.exists(), zero_stage.as_posix())
        self.assertTrue(shader_profile.exists(), shader_profile.as_posix())
        self.assertTrue(evidence_profile.exists(), evidence_profile.as_posix())

        build_text = build_script.read_text(encoding="utf-8")
        self.assertIn("apply-adreno-evidence-profile.py", build_text)

        required_files = (
            Path("framegen/public/lsfg_backend.hpp"),
            Path("framegen/src/core/device.cpp"),
            Path("framegen/v3.1_include/v3_1/context.hpp"),
            Path("framegen/v3.1_src/context.cpp"),
            Path("framegen/v3.1_include/v3_1/shaders/mipmaps.hpp"),
            Path("framegen/v3.1_src/shaders/mipmaps.cpp"),
            Path("framegen/v3.1p_include/v3_1p/context.hpp"),
            Path("framegen/v3.1p_src/context.cpp"),
            Path("framegen/v3.1p_include/v3_1p/shaders/mipmaps.hpp"),
            Path("framegen/v3.1p_src/shaders/mipmaps.cpp"),
            Path("include/context.hpp"),
            Path("include/hooks.hpp"),
            Path("src/context.cpp"),
            Path("src/hooks.cpp"),
            Path("include/extract/trans.hpp"),
            Path("src/extract/trans.cpp"),
        )

        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            for rel in required_files:
                target = temp_root / rel
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(ROOT / rel, target)

            for _ in range(2):
                subprocess.run(
                    [sys.executable, str(zero_stage), "--root", str(temp_root)],
                    check=True,
                )
                subprocess.run(
                    [sys.executable, str(shader_profile), "--root", str(temp_root)],
                    check=True,
                )
                subprocess.run(
                    [sys.executable, str(evidence_profile), "--root", str(temp_root)],
                    check=True,
                )

            backend_header = (temp_root / "framegen/public/lsfg_backend.hpp").read_text(
                encoding="utf-8"
            )
            device_source = (temp_root / "framegen/src/core/device.cpp").read_text(
                encoding="utf-8"
            )
            hooks_header = (temp_root / "include/hooks.hpp").read_text(encoding="utf-8")
            hooks_source = (temp_root / "src/hooks.cpp").read_text(encoding="utf-8")
            outer_header = (temp_root / "include/context.hpp").read_text(encoding="utf-8")
            outer_source = (temp_root / "src/context.cpp").read_text(encoding="utf-8")
            perf_header = (temp_root / "framegen/v3.1p_include/v3_1p/context.hpp").read_text(
                encoding="utf-8"
            )
            perf_source = (temp_root / "framegen/v3.1p_src/context.cpp").read_text(
                encoding="utf-8"
            )
            quality_source = (temp_root / "framegen/v3.1_src/context.cpp").read_text(
                encoding="utf-8"
            )

            self.assertIn("externalSemaphoreSyncFd", backend_header)
            self.assertIn("VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT", device_source)
            self.assertIn("externalSemaphoreSyncFd=", device_source)
            for field in ("subgroupSize=", "subgroupStages=", "subgroupOperations="):
                self.assertIn(field, device_source)

            self.assertIn("androidSyncFdSemaphoreSupported", hooks_header)
            self.assertIn("VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT", hooks_source)
            self.assertIn("syncFdSemaphore=", hooks_source)
            self.assertIn("lastDiagnosticStage()", outer_header)
            self.assertIn("present-error stage=", hooks_source)

            for field in (
                "ahb_submit_cpu_avg_ms=",
                "ahb_host_wait_avg_ms=",
                "ahb_async_submit_cpu_avg_ms=",
            ):
                self.assertIn(field, outer_source)
            self.assertIn("submitAhbHandoff(info.device", outer_source)
            self.assertIn("waitForAhbHandoff(info.device", outer_source)

            self.assertIn("generatedPreQueryPool", perf_header)
            self.assertIn("generatedPassQueryPools", perf_header)
            self.assertIn("generated-stage-profile backend=performance", perf_source)
            self.assertIn("generated-stage-profile backend=quality", quality_source)
            for field in (
                "input_transport_avg_ms=",
                "mipmaps_avg_ms=",
                "alpha_avg_ms=",
                "beta_avg_ms=",
                "gamma0_avg_ms=",
                "gamma1_avg_ms=",
                "gamma2_avg_ms=",
                "gamma3_avg_ms=",
                "gamma4_avg_ms=",
                "delta0_avg_ms=",
                "gamma5_avg_ms=",
                "delta1_avg_ms=",
                "gamma6_avg_ms=",
                "delta2_avg_ms=",
                "generate_avg_ms=",
                "output_transport_avg_ms=",
            ):
                self.assertIn(field, perf_source)

            # Profiling must use the existing command buffers/fences rather than
            # adding a new queue submission or host wait edge.
            self.assertEqual(perf_source.count("data.cmdBuffer1.submit("), 2)
            self.assertNotIn("generatedProfileFence", perf_source)


if __name__ == "__main__":
    unittest.main()
