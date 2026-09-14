from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'scripts' / 'apply-candidate-b4-beta4-predicate-canonicalization.py'

class CandidateB4Beta4PredicateTest(unittest.TestCase):
    def test_transform_exists_and_is_fail_closed_for_exact_beta4_module(self):
        text = SCRIPT.read_text(encoding='utf-8')
        self.assertIn('0x975df8da92d9c418', text)
        self.assertIn('p_beta[4]', text)
        self.assertIn('beta4-predicate-opt', text)
        self.assertIn('OpBitwiseAnd', text)
        self.assertIn('OpBitwiseOr', text)
        self.assertIn('OpIEqual', text)
        self.assertIn('removed_instructions=365', text)
        self.assertIn('kB4NewInstructions = 2404', text)
        self.assertIn('kB4NewBytes = 43564', text)

    def test_direct_masks_match_original_divisibility_for_full_workgroup(self):
        for divisor in (2, 4, 8, 16, 32):
            mask = divisor - 1
            for x in range(32):
                for y in range(32):
                    direct = (((x & mask) | (y & mask)) == 0)
                    original = (x % divisor == 0 and y % divisor == 0)
                    self.assertEqual(direct, original, (divisor, x, y))

    def test_source_patch_is_idempotent_and_runs_before_profile(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            src = root / 'src' / 'extract'
            src.mkdir(parents=True)
            trans = src / 'trans.cpp'
            trans.write_text(
                '#include <cstdint>\n#include <cstddef>\n#include <algorithm>\n'
                '#include <cstdlib>\n#include <iostream>\n#include <string>\n#include <vector>\n'
                'using namespace Extract;\n\n'
                'struct BindingOffsets {\n    uint32_t bindingIndex{};\n};\n\n'
                'std::vector<uint8_t> Extract::translateShader(\n'
                '        std::vector<uint8_t> bytecode, const std::string& shaderName) {\n'
                '    std::cerr << "lsfg-vk: shader-hot-path-opt shader=" << shaderName;\n'
                '    std::vector<uint8_t> spirvBytecode;\n'
                '    logMipmapsSpirvProfile(shaderName, spirvBytecode);\n'
                '    return spirvBytecode;\n}\n',
                encoding='utf-8',
            )
            subprocess.run([sys.executable, str(SCRIPT), '--root', str(root)], check=True)
            once = trans.read_text(encoding='utf-8')
            self.assertLess(
                once.index('applyCandidateB4Beta4Predicates(shaderName, spirvBytecode);'),
                once.index('logMipmapsSpirvProfile(shaderName, spirvBytecode);'),
            )
            subprocess.run([sys.executable, str(SCRIPT), '--root', str(root)], check=True)
            twice = trans.read_text(encoding='utf-8')
            self.assertEqual(once, twice)

    def test_android_build_is_opt_in_and_ci_exercises_b4(self):
        build = (ROOT / "scripts" / "build" / "android.sh").read_text(encoding="utf-8")
        workflow = (ROOT / ".github" / "workflows" / "android-bionic.yml").read_text(encoding="utf-8")
        flag = "LSFGVK_CANDIDATE_B4_BETA4_PREDICATES"
        self.assertIn(flag, build)
        self.assertIn("apply-candidate-b4-beta4-predicate-canonicalization.py", build)
        self.assertGreater(
            build.index("apply-candidate-b4-beta4-predicate-canonicalization.py"),
            build.index("apply-candidate-b-translation-cleanup.py"),
        )
        self.assertIn("android_candidate_b4_beta4_predicate_test.py", workflow)
        self.assertIn(flag + ': "1"', workflow)

if __name__ == '__main__':
    unittest.main()
