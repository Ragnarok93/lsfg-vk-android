#!/usr/bin/env python3
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
        header = (ROOT / "include/extract/trans.hpp").read_text(encoding="utf-8")
        translator = (ROOT / "src/extract/trans.cpp").read_text(encoding="utf-8")
        context = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn(
            "translateShader(std::vector<uint8_t> bytecode, const std::string& shaderName)",
            header,
        )
        self.assertIn('shaderName == "p_mipmaps"', translator)
        self.assertIn('shaderName == "p_beta[4]"', translator)
        self.assertIn("info.options.supportsTightIcbPacking = true", translator)
        self.assertIn("shader-hot-path-opt", translator)

        android_section = context.split("#ifdef __ANDROID__", 1)[1].split("#else", 1)[0]
        desktop_section = context.rsplit("#else", 1)[1]
        self.assertIn("Extract::translateShader(dxbc, name)", android_section)
        self.assertIn("Extract::translateShader(dxbc)", desktop_section)

        # Candidate B level-1 cleanup must not mutate algorithm topology or quality.
        for forbidden in (
            "OpExecutionMode",
            "ExecutionModeLocalSize",
            "LocalSize",
            "VK_FORMAT_R8_UNORM",
            "VK_FORMAT_R16",
            "forceComputeUavBarriers = true",
            "forceVolatileTgsmAccess = true",
        ):
            self.assertNotIn(forbidden, translator)


if __name__ == "__main__":
    unittest.main()
