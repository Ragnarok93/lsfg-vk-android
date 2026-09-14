#!/usr/bin/env python3
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidCandidateB2MipmapsDependencyContractTest(unittest.TestCase):
    def test_mipmaps_dependency_probe_is_profile_only_and_runs_after_hot_path_profile(self) -> None:
        patcher = ROOT / "scripts/apply-candidate-b2-mipmaps-dependency-profile.py"
        self.assertTrue(patcher.exists(), patcher.as_posix())

        source = patcher.read_text(encoding="utf-8")
        for token in (
            "mipmaps-barrier-profile",
            "mipmaps-phase-profile",
            "OpControlBarrier",
            "OpTypePointer",
            "OpLoad",
            "OpStore",
            "OpAccessChain",
            "OpInBoundsAccessChain",
            "OpPtrAccessChain",
            "workgroupPointerTypes",
            "workgroupPointers",
            "pre_wg_loads=",
            "pre_wg_stores=",
            "pre_image_samples=",
            "pre_image_writes=",
            "execution_scope=",
            "memory_scope=",
            "memory_semantics=0x",
        ):
            self.assertIn(token, source)

        for forbidden in (
            "supportsTightIcbPacking",
            "OpExecutionMode",
            "ExecutionModeLocalSize",
            "VK_FORMAT_",
            "vkQueueWaitIdle",
            "vkDeviceWaitIdle",
            "sleep(",
            "usleep(",
        ):
            self.assertNotIn(forbidden, source)

        build_script = (ROOT / "scripts/build/android.sh").read_text(encoding="utf-8")
        hot_path = 'python3 "${REPO_ROOT}/scripts/apply-candidate-b-shader-hot-path.py" --root "${REPO_ROOT}"'
        dependency = 'python3 "${REPO_ROOT}/scripts/apply-candidate-b2-mipmaps-dependency-profile.py" --root "${REPO_ROOT}"'
        cleanup = 'python3 "${REPO_ROOT}/scripts/apply-candidate-b-translation-cleanup.py" --root "${REPO_ROOT}"'
        self.assertIn(dependency, build_script)
        self.assertGreater(build_script.index(dependency), build_script.index(hot_path))
        self.assertLess(build_script.index(dependency), build_script.index(cleanup))

    def test_tight_icb_is_developer_opt_in_instead_of_unconditional(self) -> None:
        patcher = ROOT / "scripts/apply-candidate-b-translation-cleanup.py"
        self.assertTrue(patcher.exists(), patcher.as_posix())

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
            translator = (tmp_root / "src/extract/trans.cpp").read_text(encoding="utf-8")

        self.assertIn('std::getenv("LSFGVK_CANDIDATE_B_TIGHT_ICB")', translator)
        self.assertIn("if (optimizeHotPath && tightIcbEnabled)", translator)
        self.assertIn("info.options.supportsTightIcbPacking = true", translator)
        self.assertIn('<< " tight_icb=" << (tightIcbEnabled ? 1 : 0)', translator)
        self.assertNotIn("if (optimizeHotPath) {\n        info.options.supportsTightIcbPacking = true;", translator)


if __name__ == "__main__":
    unittest.main()
