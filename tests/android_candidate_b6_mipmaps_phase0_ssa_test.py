#!/usr/bin/env python3
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidCandidateB6MipmapsPhase0SsaContractTest(unittest.TestCase):
    def test_phase0_ssa_transform_is_exact_scoped_and_semantics_preserving(self):
        patcher = ROOT / 'scripts/apply-candidate-b6-mipmaps-phase0-ssa.py'
        self.assertTrue(patcher.exists(), patcher.as_posix())
        source = patcher.read_text(encoding='utf-8')
        required = (
            'p_mipmaps', 'candidate-b6-mipmaps-phase0-ssa',
            '0x65d3c6a69e9f9b07ULL', 'kB6ExpectedBytes = 28832',
            'kB6ExpectedBound = 1270',
            '0x56d1ebba7cb3ac76ULL', 'kB6ExpectedOutputBytes = 28052',
            'kB6ExpectedPromotedLoads = 143', 'kB6ExpectedRemovedStores = 65',
            'spv::OpCopyObject', 'spv::OpControlBarrier',
            'spv::StorageClassPrivate',
            'applyCandidateB6MipmapsPhase0Ssa(shaderName, spirvBytecode);',
        )
        for token in required:
            self.assertIn(token, source)
        forbidden = (
            'OpImageSampleExplicitLod)', 'OpImageWrite)', 'ExecutionModeLocalSize',
            'VK_FORMAT_', 'OpControlBarrier,', 'OpMemoryBarrier,',
        )
        for token in forbidden:
            self.assertNotIn(token, source)

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            trans = root / 'src/extract/trans.cpp'
            trans.parent.mkdir(parents=True)
            trans.write_text(
                '#include <cstdint>\n#include <cstddef>\n#include <algorithm>\nstruct BindingOffsets {\n    int unused;\n};\n'
                'void translated(const std::string& shaderName, std::vector<uint8_t>& spirvBytecode) {\n'
                '    applyCandidateB4Beta4PredicateCanonicalization(shaderName, spirvBytecode);\n'
                '    logMipmapsSpirvProfile(shaderName, spirvBytecode);\n}\n', encoding='utf-8')
            subprocess.run([sys.executable, str(patcher), '--root', str(root)], check=True)
            first = trans.read_text(encoding='utf-8')
            subprocess.run([sys.executable, str(patcher), '--root', str(root)], check=True)
            second = trans.read_text(encoding='utf-8')
        self.assertEqual(first, second)
        self.assertEqual(first.count('bool applyCandidateB6MipmapsPhase0Ssa('), 1)
        self.assertEqual(first.count('applyCandidateB6MipmapsPhase0Ssa(shaderName, spirvBytecode);'), 1)
        self.assertLess(first.index('applyCandidateB4Beta4PredicateCanonicalization(shaderName, spirvBytecode);'),
                        first.index('applyCandidateB6MipmapsPhase0Ssa(shaderName, spirvBytecode);'))
        self.assertLess(first.index('applyCandidateB6MipmapsPhase0Ssa(shaderName, spirvBytecode);'),
                        first.index('logMipmapsSpirvProfile(shaderName, spirvBytecode);'))

        build = (ROOT / 'scripts/build/android.sh').read_text(encoding='utf-8')
        b4 = 'python3 "${REPO_ROOT}/scripts/apply-candidate-b4-beta4-predicate.py" --root "${REPO_ROOT}"'
        b6 = 'python3 "${REPO_ROOT}/scripts/apply-candidate-b6-mipmaps-phase0-ssa.py" --root "${REPO_ROOT}"'
        self.assertIn(b6, build)
        self.assertGreater(build.index(b6), build.index(b4))
        workflow = (ROOT / '.github/workflows/android-bionic.yml').read_text(encoding='utf-8')
        self.assertIn('python3 tests/android_candidate_b6_mipmaps_phase0_ssa_test.py', workflow)


if __name__ == '__main__':
    unittest.main()
