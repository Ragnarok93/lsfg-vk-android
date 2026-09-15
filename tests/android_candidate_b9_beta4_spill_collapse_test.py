#!/usr/bin/env python3
from pathlib import Path
import subprocess, sys, tempfile, unittest

ROOT = Path(__file__).resolve().parents[1]

class B9Contract(unittest.TestCase):
    def test_b9_exists_and_is_lossless_guarded_streaming_scheduler(self):
        patcher = ROOT / "scripts/apply-candidate-b9-beta4-spill-collapse.py"
        self.assertTrue(patcher.exists(), patcher.as_posix())
        parts = ROOT / "scripts/b9_beta4_spill_collapse"
        source = patcher.read_text() + "".join(
            (parts / name).read_text()
            for name in ("part1.inc", "part2.inc", "part3.inc")
        )
        required = (
            "candidate-b9-beta4-spill-collapse",
            'shaderName != "p_beta[4]"',
            "kB9ExpectedBytes = 50764U",
            "kB9ExpectedBound = 2351U",
            "samples == 18U",
            "writes == 6U",
            "barriers == 5U",
            "local32 == 1U",
            "kB9PredicateConditions",
            "applyCandidateB9Beta4SpillCollapse(shaderName, spirvBytecode);",
            "b9ScheduleSegment",
            "b9MovableInstruction",
            "b9FunctionPointerRoot",
            "b9SampleOrder",
            "phase0",
            "OpControlBarrier",
            "OpImageSampleExplicitLod",
            "OpImageWrite",
        )
        for token in required:
            self.assertIn(token, source)

        forbidden = (
            "ExecutionModeLocalSize = 16",
            "kB9ExpectedSamples = 17",
            "OpFConvert",
            "FastMath",
        )
        for token in forbidden:
            self.assertNotIn(token, source)

    def test_b9_is_ordered_after_b4(self):
        build = (ROOT / "scripts/build/android.sh").read_text()
        b4='python3 "${REPO_ROOT}/scripts/apply-candidate-b4-beta4-predicate.py" --root "${REPO_ROOT}"'
        b9='python3 "${REPO_ROOT}/scripts/apply-candidate-b9-beta4-spill-collapse.py" --root "${REPO_ROOT}"'
        self.assertIn(b4, build)
        self.assertIn(b9, build)
        self.assertGreater(build.index(b9), build.index(b4))
        workflow = (ROOT / ".github/workflows/android-bionic.yml").read_text()
        self.assertIn("python3 tests/android_candidate_b9_beta4_spill_collapse_test.py", workflow)

    def test_b9_patcher_is_idempotent_and_chains_after_b4(self):
        patcher = ROOT / "scripts/apply-candidate-b9-beta4-spill-collapse.py"
        self.assertTrue(patcher.exists(), patcher.as_posix())
        with tempfile.TemporaryDirectory() as td:
            tr = Path(td) / "src/extract/trans.cpp"
            tr.parent.mkdir(parents=True)
            tr.write_text(
                '#include <cstddef>\n#include <cstring>\n'
                'struct BindingOffsets {\n    int unused;\n};\n'
                'void translated(const std::string& shaderName, std::vector<uint8_t>& spirvBytecode) {\n'
                '    applyCandidateB4Beta4PredicateCanonicalization(shaderName, spirvBytecode);\n'
                '    logMipmapsSpirvProfile(shaderName, spirvBytecode);\n'
                '}\n'
            )
            subprocess.run([sys.executable, str(patcher), "--root", td], check=True)
            first=tr.read_text()
            subprocess.run([sys.executable, str(patcher), "--root", td], check=True)
            second=tr.read_text()
            self.assertEqual(first, second)
            self.assertEqual(first.count("bool applyCandidateB9Beta4SpillCollapse("), 1)
            self.assertEqual(first.count("applyCandidateB9Beta4SpillCollapse(shaderName, spirvBytecode);"), 1)
            self.assertLess(
                first.index("applyCandidateB4Beta4PredicateCanonicalization(shaderName, spirvBytecode);"),
                first.index("applyCandidateB9Beta4SpillCollapse(shaderName, spirvBytecode);"),
            )
            self.assertLess(
                first.index("applyCandidateB9Beta4SpillCollapse(shaderName, spirvBytecode);"),
                first.index("logMipmapsSpirvProfile(shaderName, spirvBytecode);"),
            )

if __name__ == "__main__":
    unittest.main()
