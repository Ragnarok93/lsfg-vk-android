#!/usr/bin/env python3
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

class AndroidCandidateB5MipmapsPredicateContractTest(unittest.TestCase):
    def test_mipmaps_predicate_transform_is_exact_fingerprinted_and_semantics_preserving(self):
        patcher = ROOT / 'scripts/apply-candidate-b5-mipmaps-predicate.py'
        self.assertTrue(patcher.exists(), patcher.as_posix())
        source = patcher.read_text(encoding='utf-8')
        required = (
            'p_mipmaps', 'candidate-b5-mipmaps-predicate-opt', '0x65d3c6a69e9f9b07ULL',
            'kB5ExpectedBytes = 28832', 'kB5ExpectedBound = 1270',
            'kB5ExpectedOutputFnv1a64 = 0xdcd905da4b4b6edcULL',
            'kB5ExpectedOutputBytes = 29612', 'kB5ExpectedOutputBound = 1310',
            'kB5PredicateCount = 5',
            '670U, 790U, 931U, 1050U, 1190U',
            '11U, 699U, 819U, 960U, 1079U',
            '672U, 792U, 933U, 1052U, 1192U',
            '671U, 791U, 932U, 1051U, 1191U',
            'kB5TypeV3Uint = 32', 'kB5TypeUint = 10', 'kB5TypeBool = 48',
            'kB5LocalInvocationId = 34', 'kB5Zero = 40',
            'spv::OpLoad', 'spv::OpCompositeExtract', 'spv::OpUMod', 'spv::OpIEqual',
            'spv::OpLogicalAnd', 'spv::OpSelectionMerge', 'spv::OpBranchConditional',
            'applyCandidateB5MipmapsPredicateCanonicalization(shaderName, spirvBytecode);',
        )
        for token in required:
            self.assertIn(token, source)
        forbidden = (
            'OpControlBarrier)', 'OpImageSampleExplicitLod)', 'OpImageWrite)',
            'ExecutionModeLocalSize', 'VK_FORMAT_', 'supportsTightIcbPacking',
            'LSFGVK_CANDIDATE_B_TIGHT_ICB',
        )
        for token in forbidden:
            self.assertNotIn(token, source)

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            trans = root / 'src/extract/trans.cpp'
            trans.parent.mkdir(parents=True)
            trans.write_text(
                '#include <cstddef>\n#include <algorithm>\nstruct BindingOffsets {\n    int unused;\n};\n'
                'void translated(const std::string& shaderName, std::vector<uint8_t>& spirvBytecode) {\n'
                '    applyCandidateB4Beta4PredicateCanonicalization(shaderName, spirvBytecode);\n'
                '    logMipmapsSpirvProfile(shaderName, spirvBytecode);\n}\n', encoding='utf-8')
            subprocess.run([sys.executable, str(patcher), '--root', str(root)], check=True)
            first = trans.read_text(encoding='utf-8')
            subprocess.run([sys.executable, str(patcher), '--root', str(root)], check=True)
            second = trans.read_text(encoding='utf-8')
        self.assertEqual(first, second)
        self.assertEqual(first.count('bool applyCandidateB5MipmapsPredicateCanonicalization('), 1)
        self.assertEqual(first.count('applyCandidateB5MipmapsPredicateCanonicalization(shaderName, spirvBytecode);'), 1)
        self.assertLess(first.index('applyCandidateB4Beta4PredicateCanonicalization(shaderName, spirvBytecode);'),
                        first.index('applyCandidateB5MipmapsPredicateCanonicalization(shaderName, spirvBytecode);'))
        self.assertLess(first.index('applyCandidateB5MipmapsPredicateCanonicalization(shaderName, spirvBytecode);'),
                        first.index('logMipmapsSpirvProfile(shaderName, spirvBytecode);'))

        build = (ROOT / 'scripts/build/android.sh').read_text(encoding='utf-8')
        b4 = 'python3 "${REPO_ROOT}/scripts/apply-candidate-b4-beta4-predicate.py" --root "${REPO_ROOT}"'
        b5 = 'python3 "${REPO_ROOT}/scripts/apply-candidate-b5-mipmaps-predicate.py" --root "${REPO_ROOT}"'
        self.assertIn(b5, build)
        self.assertGreater(build.index(b5), build.index(b4))
        workflow = (ROOT / '.github/workflows/android-bionic.yml').read_text(encoding='utf-8')
        self.assertIn('python3 tests/android_candidate_b5_mipmaps_predicate_test.py', workflow)

if __name__ == '__main__':
    unittest.main()
