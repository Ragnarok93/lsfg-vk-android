#!/usr/bin/env python3
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidCandidateB3Beta4AnalysisContractTest(unittest.TestCase):
    def test_beta4_dependency_probe_is_observational_and_composes_after_b2(self) -> None:
        patcher = ROOT / "scripts/apply-candidate-b3-beta4-analysis.py"
        self.assertTrue(patcher.exists(), patcher.as_posix())
        source = patcher.read_text(encoding="utf-8")
        for token in (
            "p_beta[4]", "beta4-barrier-profile", "beta4-phase-profile",
            "beta4-spirv-chunk", "OpControlBarrier", "OpLoad", "OpStore",
            "wg_loads=", "wg_stores=", "image_samples=", "image_writes=",
            "execution_scope=", "memory_scope=", "memory_semantics=0x",
        ):
            self.assertIn(token, source)
        for forbidden in (
            "supportsTightIcbPacking", "ExecutionModeLocalSize", "VK_FORMAT_",
            "vkQueueWaitIdle", "vkDeviceWaitIdle", "setenv(", "unsetenv(",
        ):
            self.assertNotIn(forbidden, source)

        with tempfile.TemporaryDirectory() as tmp:
            tmp_root = Path(tmp)
            trans = tmp_root / "src/extract/trans.cpp"
            trans.parent.mkdir(parents=True)
            trans.write_text(
                "namespace {\n"
                "    // mipmaps-barrier-profile\n"
                "    constexpr uint16_t kB2OpLoad = 61;\n"
                "    constexpr uint16_t kB2OpStore = 62;\n"
                "    bool isB2WorkgroupPointerProducer(uint16_t) { return false; }\n"
                "    uint64_t b2ConstantValue(const std::unordered_map<uint32_t, uint64_t>&, uint32_t) { return 0; }\n"
                "    void logMipmapsSpirvProfile(\n"
                "            const std::string&, const std::vector<uint8_t>&) {}\n"
                "}\n"
                "void translated(const std::string& shaderName, std::vector<uint8_t>& spirvBytecode) {\n"
                "    logMipmapsSpirvProfile(shaderName, spirvBytecode);\n"
                "    return;\n"
                "}\n",
                encoding="utf-8",
            )
            subprocess.run([sys.executable, str(patcher), "--root", str(tmp_root)], check=True)
            first = trans.read_text(encoding="utf-8")
            subprocess.run([sys.executable, str(patcher), "--root", str(tmp_root)], check=True)
            second = trans.read_text(encoding="utf-8")

        self.assertEqual(first, second)
        self.assertEqual(first.count("void logBeta4DependencyProfile("), 1)
        self.assertEqual(first.count("logBeta4DependencyProfile(shaderName, spirvBytecode);"), 1)
        self.assertLess(
            first.index("logMipmapsSpirvProfile(shaderName, spirvBytecode);"),
            first.index("logBeta4DependencyProfile(shaderName, spirvBytecode);"),
        )

        build = (ROOT / "scripts/build/android.sh").read_text(encoding="utf-8")
        b2 = 'python3 "${REPO_ROOT}/scripts/apply-candidate-b2-mipmaps-dependency-profile.py" --root "${REPO_ROOT}"'
        b3 = 'python3 "${REPO_ROOT}/scripts/apply-candidate-b3-beta4-analysis.py" --root "${REPO_ROOT}"'
        cleanup = 'python3 "${REPO_ROOT}/scripts/apply-candidate-b-translation-cleanup.py" --root "${REPO_ROOT}"'
        self.assertIn(b3, build)
        self.assertGreater(build.index(b3), build.index(b2))
        self.assertLess(build.index(b3), build.index(cleanup))

        workflow = (ROOT / ".github/workflows/android-bionic.yml").read_text(encoding="utf-8")
        self.assertIn("python3 tests/android_candidate_b3_beta4_analysis_test.py", workflow)


if __name__ == "__main__":
    unittest.main()
