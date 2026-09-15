#!/usr/bin/env python3
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidCandidateB7MipmapsLiveRangeContractTest(unittest.TestCase):
    def test_live_range_reorder_is_exact_scoped_and_composed_after_b4(self):
        patcher = ROOT / 'scripts/apply-candidate-b7-mipmaps-live-range.py'
        self.assertTrue(patcher.exists(), patcher.as_posix())
        source = patcher.read_text(encoding='utf-8')

        required = (
            'p_mipmaps',
            'candidate-b7-mipmaps-live-range',
            '0x65d3c6a69e9f9b07ULL',
            'kB7ExpectedInputBytes = 28832',
            'kB7ExpectedInputBound = 1270',
            '0x56d1ebba7cb3ac76ULL',
            'kB7ExpectedSsaBytes = 28052',
            '0xa8153e454b8da00aULL',
            'kB7ExpectedOutputBytes = 28076',
            'kB7OutputBound = 1271',
            'kB7ExpectedPromotedLoads = 143',
            'kB7ExpectedRemovedStores = 65',
            'kB7FlagCarrierId = 1270',
            'kB7AStart = 295',
            'kB7BStart = 302',
            'kB7CStart = 313',
            'kB7DStart = 324',
            'kB7ConditionStart = 335',
            'kB7ABlockStart = 345',
            'kB7BBlockStart = 423',
            'kB7CBlockStart = 501',
            'kB7DBlockStart = 579',
            '398U, 476U, 554U',
            'applyCandidateB7MipmapsLiveRange(shaderName, spirvBytecode);',
        )
        for token in required:
            self.assertIn(token, source)

        # B7 may reorder whole phase-0 instructions and patch only three
        # CopyObject carrier operands plus one new CompositeInsert. It must not
        # rewrite sampling, image-write, barrier, local-size, format or precision
        # opcodes themselves.
        forbidden = (
            'ExecutionModeLocalSize',
            'VK_FORMAT_',
            'OpMemoryBarrier',
            'OpImageSampleExplicitLod =',
            'OpImageWrite =',
        )
        for token in forbidden:
            self.assertNotIn(token, source)

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            trans = root / 'src/extract/trans.cpp'
            trans.parent.mkdir(parents=True)
            trans.write_text(
                '#include <cstdint>\n#include <cstddef>\n#include <algorithm>\n'
                'struct BindingOffsets {\n    int unused;\n};\n'
                'void translated(const std::string& shaderName, std::vector<uint8_t>& spirvBytecode) {\n'
                '    applyCandidateB4Beta4PredicateCanonicalization(shaderName, spirvBytecode);\n'
                '    logMipmapsSpirvProfile(shaderName, spirvBytecode);\n'
                '}\n',
                encoding='utf-8',
            )
            subprocess.run([sys.executable, str(patcher), '--root', str(root)], check=True)
            first = trans.read_text(encoding='utf-8')
            subprocess.run([sys.executable, str(patcher), '--root', str(root)], check=True)
            second = trans.read_text(encoding='utf-8')

        self.assertEqual(first, second)
        self.assertEqual(first.count('bool applyCandidateB7MipmapsLiveRange('), 1)
        self.assertEqual(
            first.count('applyCandidateB7MipmapsLiveRange(shaderName, spirvBytecode);'), 1
        )
        self.assertLess(
            first.index('applyCandidateB4Beta4PredicateCanonicalization(shaderName, spirvBytecode);'),
            first.index('applyCandidateB7MipmapsLiveRange(shaderName, spirvBytecode);'),
        )
        self.assertLess(
            first.index('applyCandidateB7MipmapsLiveRange(shaderName, spirvBytecode);'),
            first.index('logMipmapsSpirvProfile(shaderName, spirvBytecode);'),
        )

        build = (ROOT / 'scripts/build/android.sh').read_text(encoding='utf-8')
        b4 = 'python3 "${REPO_ROOT}/scripts/apply-candidate-b4-beta4-predicate.py" --root "${REPO_ROOT}"'
        b7 = 'python3 "${REPO_ROOT}/scripts/apply-candidate-b7-mipmaps-live-range.py" --root "${REPO_ROOT}"'
        self.assertIn(b7, build)
        self.assertGreater(build.index(b7), build.index(b4))

        workflow = (ROOT / '.github/workflows/android-bionic.yml').read_text(encoding='utf-8')
        self.assertIn(
            'python3 tests/android_candidate_b7_mipmaps_live_range_test.py', workflow
        )


if __name__ == '__main__':
    unittest.main()
