#!/usr/bin/env python3
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidCandidateBShaderHotPathContractTest(unittest.TestCase):
    def test_candidate_b_profiles_mipmaps_and_each_beta_pass_without_sync_or_pacing_changes(self) -> None:
        patcher = ROOT / "scripts/apply-candidate-b-shader-hot-path.py"
        self.assertTrue(patcher.exists(), patcher.as_posix())

        build_script = (ROOT / "scripts/build/android.sh").read_text(encoding="utf-8")
        invocation = 'python3 "${REPO_ROOT}/scripts/apply-candidate-b-shader-hot-path.py" --root "${REPO_ROOT}"'
        self.assertIn(invocation, build_script)
        self.assertGreater(
            build_script.index(invocation),
            build_script.index('python3 "${REPO_ROOT}/scripts/apply-adreno-evidence-profile.py"'),
        )

        source = patcher.read_text(encoding="utf-8")
        for token in (
            "shader-hot-path-profile",
            'shaderName == "mipmaps"',
            'shaderName == "p_mipmaps"',
            'shaderName.rfind("beta[", 0)',
            'shaderName.rfind("p_beta[", 0)',
            "Core::TimestampQueryPool* profilePool",
            "postPassQueryIndex",
            "std::array<double, 8> generatedPreProfileTotalsMs",
            "beta0_avg_ms=",
            "beta1_avg_ms=",
            "beta2_avg_ms=",
            "beta3_avg_ms=",
            "beta4_avg_ms=",
        ):
            self.assertIn(token, source)

        for forbidden in (
            "sleep(",
            "usleep(",
            "vkQueueWaitIdle",
            "vkDeviceWaitIdle",
            "vkImportSemaphoreFdKHR",
            "XServerScreen",
        ):
            self.assertNotIn(forbidden, source)

    def test_candidate_b_translation_cleanup_is_android_only_and_hot_path_scoped(self) -> None:
        patcher = ROOT / "scripts/apply-candidate-b-translation-cleanup.py"
        self.assertTrue(patcher.exists(), patcher.as_posix())
        patch_source = patcher.read_text(encoding="utf-8")

        for token in (
            'shaderName == "p_mipmaps"',
            'shaderName == "p_beta[4]"',
            "info.options.supportsTightIcbPacking = true",
            "shader-hot-path-opt",
        ):
            self.assertIn(token, patch_source)

        with tempfile.TemporaryDirectory() as tmp:
            tmp_root = Path(tmp)
            (tmp_root / "src/extract").mkdir(parents=True)
            (tmp_root / "include/extract").mkdir(parents=True)
            shutil.copy2(ROOT / "src/extract/trans.cpp", tmp_root / "src/extract/trans.cpp")
            shutil.copy2(ROOT / "include/extract/trans.hpp", tmp_root / "include/extract/trans.hpp")
            shutil.copy2(ROOT / "src/context.cpp", tmp_root / "src/context.cpp")
            subprocess.run(
                [sys.executable, str(patcher), "--root", str(tmp_root)],
                check=True,
            )
            header = (tmp_root / "include/extract/trans.hpp").read_text(encoding="utf-8")
            translator = (tmp_root / "src/extract/trans.cpp").read_text(encoding="utf-8")
            context = (tmp_root / "src/context.cpp").read_text(encoding="utf-8")

            self.assertIn("const std::string& shaderName", header)
            self.assertIn('shaderName == "p_mipmaps"', translator)
            self.assertIn('shaderName == "p_beta[4]"', translator)
            self.assertIn("info.options.supportsTightIcbPacking = true", translator)
            self.assertEqual(context.count("Extract::translateShader(dxbc, name)"), 1)
            self.assertGreaterEqual(context.count("Extract::translateShader(dxbc)"), 1)

        build_script = (ROOT / "scripts/build/android.sh").read_text(encoding="utf-8")
        invocation = 'python3 "${REPO_ROOT}/scripts/apply-candidate-b-translation-cleanup.py" --root "${REPO_ROOT}"'
        profile_invocation = 'python3 "${REPO_ROOT}/scripts/apply-candidate-b-shader-hot-path.py" --root "${REPO_ROOT}"'
        self.assertIn(invocation, build_script)
        self.assertGreater(build_script.index(invocation), build_script.index(profile_invocation))

        # Candidate B level-1 cleanup must not mutate algorithm topology or quality.
        for forbidden in (
            "OpExecutionMode",
            "ExecutionModeLocalSize",
            "LocalSize",
            "VK_FORMAT_R8_UNORM",
            "VK_FORMAT_R16",
            "forceComputeUavBarriers = true",
            "forceVolatileTgsmAccess = true",
            "sleep(",
            "vkQueueWaitIdle",
            "vkDeviceWaitIdle",
        ):
            self.assertNotIn(forbidden, patch_source)


if __name__ == "__main__":
    unittest.main()
