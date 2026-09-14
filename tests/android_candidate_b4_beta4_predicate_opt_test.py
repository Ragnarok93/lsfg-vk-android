#!/usr/bin/env python3
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidCandidateB4Beta4PredicateOptContractTest(unittest.TestCase):
    def test_beta4_predicate_optimizer_is_exact_fingerprint_guarded_and_build_ordered(self) -> None:
        patcher = ROOT / "scripts/apply-candidate-b4-beta4-predicate-opt.py"
        self.assertTrue(patcher.exists(), patcher.as_posix())

        source = patcher.read_text(encoding="utf-8")
        for token in (
            'p_beta[4]',
            '0x975df8da92d9c418ULL',
            '0xcc6ef6a9a87ef769ULL',
            'kB4ExpectedInputBytes = 49984',
            'kB4ExpectedOutputBytes = 44384',
            'kB4ExpectedStages = 5',
            'kB4ExpectedRemovedInstructionsPerStage = 81',
            'beta4-predicate-opt',
            'spv::OpUMod',
            'spv::OpIEqual',
            'spv::OpLogicalAnd',
            'spv::OpSelect',
            'spv::OpCompositeInsert',
        ):
            self.assertIn(token, source)

        for forbidden in (
            'OpImageSample',
            'OpImageWrite',
            'OpControlBarrier,',
            'ExecutionModeLocalSize',
            'VK_FORMAT_',
            'supportsTightIcbPacking = true',
        ):
            self.assertNotIn(forbidden, source)

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            trans = root / 'src/extract/trans.cpp'
            trans.parent.mkdir(parents=True)
            trans.write_text(
                '#include "extract/trans.hpp"\n'
                '#include <thirdparty/spirv.hpp>\n'
                '#include <algorithm>\n'
                '#include <vector>\n\n'
                'std::vector<uint8_t> Extract::translateShader(\n'
                '        std::vector<uint8_t> bytecode, const std::string& shaderName) {\n'
                '    std::vector<uint8_t> spirvBytecode = bytecode;\n'
                '    logBeta4DependencyProfile(shaderName, spirvBytecode);\n'
                '    return spirvBytecode;\n'
                '}\n',
                encoding='utf-8',
            )
            subprocess.run([sys.executable, str(patcher), '--root', str(root)], check=True)
            first = trans.read_text(encoding='utf-8')
            subprocess.run([sys.executable, str(patcher), '--root', str(root)], check=True)
            second = trans.read_text(encoding='utf-8')

        self.assertEqual(first, second)
        self.assertEqual(first.count('bool optimizeBeta4ReductionPredicates('), 1)
        self.assertEqual(first.count('optimizeBeta4ReductionPredicates(shaderName, spirvBytecode);'), 1)
        self.assertLess(
            first.index('logBeta4DependencyProfile(shaderName, spirvBytecode);'),
            first.index('optimizeBeta4ReductionPredicates(shaderName, spirvBytecode);'),
        )

        build = (ROOT / 'scripts/build/android.sh').read_text(encoding='utf-8')
        b3 = 'python3 "${REPO_ROOT}/scripts/apply-candidate-b3-beta4-analysis.py" --root "${REPO_ROOT}"'
        cleanup = 'python3 "${REPO_ROOT}/scripts/apply-candidate-b-translation-cleanup.py" --root "${REPO_ROOT}"'
        b4 = 'python3 "${REPO_ROOT}/scripts/apply-candidate-b4-beta4-predicate-opt.py" --root "${REPO_ROOT}"'
        self.assertIn(b4, build)
        self.assertGreater(build.index(b4), build.index(cleanup))
        self.assertGreater(build.index(cleanup), build.index(b3))

        workflow = (ROOT / '.github/workflows/android-bionic.yml').read_text(encoding='utf-8')
        self.assertIn('python3 tests/android_candidate_b4_beta4_predicate_opt_test.py', workflow)


if __name__ == '__main__':
    unittest.main()
