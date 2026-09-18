#!/usr/bin/env python3
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class AndroidCandidateB13Beta4FusedMaskContractTest(unittest.TestCase):
    def test_fused_mask_identity_is_exact_for_all_uint8_coordinates(self) -> None:
        for step in (2, 4, 8, 16, 32):
            mask = step - 1
            for x in range(256):
                for y in range(256):
                    baseline = (x & mask) == 0 and (y & mask) == 0
                    fused = ((x | y) & mask) == 0
                    self.assertEqual(baseline, fused, (step, x, y))

    def test_b13_hoists_coordinates_and_fuses_pow2_predicates(self) -> None:
        patcher = ROOT / "scripts/apply-candidate-b13-beta4-fused-mask.py"
        self.assertTrue(patcher.exists(), patcher.as_posix())
        source = patcher.read_text(encoding="utf-8")
        for token in (
            "candidate-b13-beta4-fused-mask",
            "spv::OpBitwiseOr",
            "kB13OpBitwiseOr",
            "b13SharedLocalId",
            "b13SharedX",
            "b13SharedY",
            "b13SharedXY",
            "b13CoordinatesEmitted",
            "b4AppendOp4(rewritten, kB13OpBitwiseOr",
            "b4AppendOp4(rewritten, kB11OpBitwiseAnd",
            "b4AppendOp4(rewritten, kB4OpIEqual",
            "b11MaskConstants[predicateIndex]",
            '<< " fallback=" << (b11Enabled ? "none" : "b4")',
        ):
            self.assertIn(token, source)

        for forbidden in (
            "spv::OpControlBarrier",
            "spv::OpImageSampleExplicitLod",
            "spv::OpImageWrite",
            "ExecutionModeLocalSize",
            "VK_FORMAT_",
            "vendorID",
            "deviceID",
            "Adreno",
            "Turnip",
            "Mali",
        ):
            self.assertNotIn(forbidden, source)

        b4 = ROOT / "scripts/apply-candidate-b4-beta4-predicate.py"
        b11 = ROOT / "scripts/apply-candidate-b11-beta4-pow2-mask.py"
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            trans = root / "src/extract/trans.cpp"
            trans.parent.mkdir(parents=True)
            trans.write_text(
                "#include <cstddef>\n#include <algorithm>\nstruct BindingOffsets {\n int unused;\n};\n"
                "std::vector<uint8_t> translated(const std::string& shaderName, "
                "std::vector<uint8_t> spirvBytecode) {\n    return spirvBytecode;\n}\n",
                encoding="utf-8",
            )
            for transform in (b4, b11, patcher, patcher):
                subprocess.run(
                    [sys.executable, str(transform), "--root", str(root)], check=True
                )
            transformed = trans.read_text(encoding="utf-8")

        self.assertEqual(transformed.count("candidate-b13-beta4-fused-mask"), 2)
        self.assertEqual(transformed.count("constexpr uint16_t kB13OpBitwiseOr"), 1)
        self.assertEqual(transformed.count("uint32_t b13SharedLocalId = 0U;"), 1)
        self.assertEqual(transformed.count("uint32_t b13SharedXY = 0U;"), 1)
        self.assertIn("if (b11Enabled)", transformed)
        self.assertIn("else {\n                    const uint32_t localId", transformed)

        build = (ROOT / "scripts/build/android.sh").read_text(encoding="utf-8")
        b11_call = (
            'python3 "${REPO_ROOT}/scripts/apply-candidate-b11-beta4-pow2-mask.py" '
            '--root "${REPO_ROOT}"'
        )
        b13_call = (
            'python3 "${REPO_ROOT}/scripts/apply-candidate-b13-beta4-fused-mask.py" '
            '--root "${REPO_ROOT}"'
        )
        self.assertIn(b13_call, build)
        self.assertGreater(build.index(b13_call), build.index(b11_call))
        self.assertNotIn("B11_PROFILE_VARIANT", build)


if __name__ == "__main__":
    unittest.main()
