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
        b11_stage_profile = ROOT / "scripts/apply-b11-beta4-stage-profile.py"
        syncfd_profile = ROOT / "scripts/adreno_syncfd_handoff.py"
        build_script = ROOT / "scripts/build/android.sh"

        self.assertTrue(zero_stage.exists(), zero_stage.as_posix())
        self.assertTrue(shader_profile.exists(), shader_profile.as_posix())
        self.assertTrue(evidence_profile.exists(), evidence_profile.as_posix())
        self.assertTrue(b11_stage_profile.exists(), b11_stage_profile.as_posix())
        self.assertTrue(syncfd_profile.exists(), syncfd_profile.as_posix())

        build_text = build_script.read_text(encoding="utf-8")
        self.assertIn("apply-adreno-evidence-profile.py", build_text)
        self.assertIn("LSFGVK_B11_EVIDENCE_PROFILE", build_text)
        self.assertIn("LSFGVK_B11_PROFILE_VARIANT", build_text)
        self.assertIn("apply-b11-beta4-stage-profile.py", build_text)
        self.assertIn("apply-candidate-b6-pipeline-executable-profile.py", build_text)
        self.assertIn('"${B11_PROFILE_VARIANT}" == "b4"', build_text)

        required_files = (
            Path("framegen/public/lsfg_backend.hpp"),
            Path("framegen/public/lsfg_3_1.hpp"),
            Path("framegen/public/lsfg_3_1p.hpp"),
            Path("framegen/src/core/device.cpp"),
            Path("framegen/include/core/semaphore.hpp"),
            Path("framegen/src/core/semaphore.cpp"),
            Path("framegen/include/core/timestampquerypool.hpp"),
            Path("framegen/src/core/timestampquerypool.cpp"),
            Path("framegen/v3.1_include/v3_1/context.hpp"),
            Path("framegen/v3.1_src/context.cpp"),
            Path("framegen/v3.1_src/lsfg.cpp"),
            Path("framegen/v3.1_include/v3_1/shaders/mipmaps.hpp"),
            Path("framegen/v3.1_src/shaders/mipmaps.cpp"),
            Path("framegen/v3.1_include/v3_1/shaders/beta.hpp"),
            Path("framegen/v3.1_src/shaders/beta.cpp"),
            Path("framegen/v3.1p_include/v3_1p/context.hpp"),
            Path("framegen/v3.1p_src/context.cpp"),
            Path("framegen/v3.1p_src/lsfg.cpp"),
            Path("framegen/v3.1p_include/v3_1p/shaders/mipmaps.hpp"),
            Path("framegen/v3.1p_src/shaders/mipmaps.cpp"),
            Path("framegen/v3.1p_include/v3_1p/shaders/beta.hpp"),
            Path("framegen/v3.1p_src/shaders/beta.cpp"),
            Path("include/context.hpp"),
            Path("include/hooks.hpp"),
            Path("include/mini/semaphore.hpp"),
            Path("src/context.cpp"),
            Path("src/hooks.cpp"),
            Path("src/mini/semaphore.cpp"),
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
                subprocess.run(
                    [sys.executable, str(b11_stage_profile), "--root", str(temp_root)],
                    check=True,
                )

            backend_header = (temp_root / "framegen/public/lsfg_backend.hpp").read_text(
                encoding="utf-8"
            )
            public_headers = (
                (temp_root / "framegen/public/lsfg_3_1.hpp").read_text(encoding="utf-8"),
                (temp_root / "framegen/public/lsfg_3_1p.hpp").read_text(encoding="utf-8"),
            )
            public_sources = (
                (temp_root / "framegen/v3.1_src/lsfg.cpp").read_text(encoding="utf-8"),
                (temp_root / "framegen/v3.1p_src/lsfg.cpp").read_text(encoding="utf-8"),
            )
            device_source = (temp_root / "framegen/src/core/device.cpp").read_text(
                encoding="utf-8"
            )
            core_semaphore = (temp_root / "framegen/src/core/semaphore.cpp").read_text(
                encoding="utf-8"
            )
            hooks_header = (temp_root / "include/hooks.hpp").read_text(encoding="utf-8")
            hooks_source = (temp_root / "src/hooks.cpp").read_text(encoding="utf-8")
            outer_header = (temp_root / "include/context.hpp").read_text(encoding="utf-8")
            outer_source = (temp_root / "src/context.cpp").read_text(encoding="utf-8")
            mini_header = (temp_root / "include/mini/semaphore.hpp").read_text(encoding="utf-8")
            mini_source = (temp_root / "src/mini/semaphore.cpp").read_text(encoding="utf-8")
            perf_header = (temp_root / "framegen/v3.1p_include/v3_1p/context.hpp").read_text(
                encoding="utf-8"
            )
            perf_source = (temp_root / "framegen/v3.1p_src/context.cpp").read_text(
                encoding="utf-8"
            )
            quality_source = (temp_root / "framegen/v3.1_src/context.cpp").read_text(
                encoding="utf-8"
            )
            perf_beta_header = (
                temp_root / "framegen/v3.1p_include/v3_1p/shaders/beta.hpp"
            ).read_text(encoding="utf-8")
            perf_beta_source = (
                temp_root / "framegen/v3.1p_src/shaders/beta.cpp"
            ).read_text(encoding="utf-8")

            self.assertIn("externalSemaphoreSyncFd", backend_header)
            self.assertIn("VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT", device_source)
            self.assertIn("externalSemaphoreSyncFd=", device_source)
            self.assertIn("diagnostics.externalSemaphoreSyncFd", device_source)
            for field in ("subgroupSize=", "subgroupStages=", "subgroupOperations="):
                self.assertIn(field, device_source)

            self.assertIn("androidSyncFdSemaphoreSupported", hooks_header)
            self.assertIn("VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT", hooks_source)
            self.assertIn("syncFdSemaphore=", hooks_source)
            self.assertIn('"sync-fd"', hooks_source)
            self.assertIn("opaqueFdSemaphoreSupported || syncFdSemaphoreSupported", hooks_source)
            self.assertIn("lastDiagnosticStage()", outer_header)
            self.assertIn("present-error stage=", hooks_source)

            self.assertIn("asyncAhbHandoffHandleType_", outer_header)
            self.assertIn("VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT", outer_source)
            self.assertIn("gpu-sync-fd", outer_source)
            self.assertIn("int exportFd(", mini_header)
            self.assertIn("VkExternalSemaphoreHandleTypeFlagBits handleType", mini_source)
            self.assertIn("VK_SEMAPHORE_IMPORT_TEMPORARY_BIT", core_semaphore)
            self.assertIn("VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT", core_semaphore)

            for public_header in public_headers:
                self.assertIn("presentContextWithCountAndHistoryFd", public_header)
                self.assertIn(
                    '__attribute__((visibility("default")))\n    int presentContextWithCountAndHistoryFd',
                    public_header,
                )
                self.assertIn(
                    '__attribute__((visibility("default")))\n    bool waitContext',
                    public_header,
                )
            for public_source in public_sources:
                self.assertIn("presentContextWithCountAndHistoryFd", public_source)

            submit_pos = outer_source.index("submitAhbHandoff(info.device")
            export_pos = outer_source.index("framegenInputSemaphore.exportFd(", submit_pos)
            dispatch_pos = outer_source.index("presentContextWithCount", export_pos)
            self.assertLess(submit_pos, export_pos,
                "SYNC_FD export must happen only after its queue signal is pending")
            self.assertLess(export_pos, dispatch_pos,
                "framegen must receive the exported synchronization payload after submission")

            for field in (
                "ahb_submit_cpu_avg_ms=",
                "ahb_host_wait_avg_ms=",
                "ahb_async_submit_cpu_avg_ms=",
                "source_copy_gpu_avg_ms=",
            ):
                self.assertIn(field, outer_source)
            self.assertIn("gameSourceCopyQueryPool", outer_header)
            self.assertIn("VK_QUERY_TYPE_TIMESTAMP", outer_source)
            self.assertIn("VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT", outer_source)
            self.assertIn("VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT", outer_source)
            self.assertIn("VK_QUERY_RESULT_64_BIT", outer_source)
            self.assertIn("submitAhbHandoff(info.device", outer_source)
            self.assertIn("waitForAhbHandoff(info.device", outer_source)

            self.assertIn("generatedPreQueryPool", perf_header)
            self.assertIn("generatedPassQueryPools", perf_header)
            self.assertIn("generatedBeta4QueryPool", perf_header)
            self.assertIn("generatedBeta4ProfileTotalMs", perf_header)
            self.assertIn("generated-stage-profile backend=performance", perf_source)
            self.assertIn("generated-stage-profile backend=quality", quality_source)
            for field in (
                "input_transport_avg_ms=",
                "mipmaps_avg_ms=",
                "alpha_avg_ms=",
                "beta_avg_ms=",
                "beta4_profile_samples=",
                "beta4_avg_ms=",
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

            self.assertIn("Core::TimestampQueryPool* beta4Profile", perf_beta_header)
            self.assertIn("beta4Profile->reset(buf.handle())", perf_beta_source)
            self.assertIn("beta4Profile->write(buf.handle(), 0)", perf_beta_source)
            self.assertIn("beta4Profile->write(buf.handle(), 1)", perf_beta_source)
            self.assertEqual(perf_beta_source.count("beta4Profile->write"), 2)
            fifth_pass = perf_beta_source.index("// fifth pass")
            beta4_reset = perf_beta_source.index("beta4Profile->reset", fifth_pass)
            beta4_bind = perf_beta_source.index("this->pipelines.at(4).bind", beta4_reset)
            beta4_end = perf_beta_source.index("beta4Profile->write(buf.handle(), 1)", beta4_bind)
            self.assertLess(beta4_reset, beta4_bind)
            self.assertLess(beta4_bind, beta4_end)

            # Profiling keeps the established submits and adds exactly one
            # TransportOnly preprocessing submit behind an internal semaphore.
            # The exported history-completion semaphore is signaled by the
            # preceding shared-AHB release submit, not by private mipmaps/alpha.
            self.assertEqual(perf_source.count("data.cmdBuffer1.submit("), 4)
            self.assertIn("data.transportReleaseCommandBuffer.submit(", perf_source)
            self.assertIn("{data.historyCompletionSemaphore,data.transportReadySemaphore}", perf_source)
            self.assertIn("{data.transportReadySemaphore}", perf_source)
            self.assertIn("historyCompletionSemaphore", perf_source)
            self.assertNotIn("generatedProfileFence", perf_source)
            self.assertNotIn("sourceCopyProfileFence", outer_source)


if __name__ == "__main__":
    unittest.main()
