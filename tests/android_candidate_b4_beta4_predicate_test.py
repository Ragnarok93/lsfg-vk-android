#!/usr/bin/env python3
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidCandidateB4Beta4PredicateContractTest(unittest.TestCase):
    def test_beta4_predicate_transform_is_exact_fingerprinted_and_semantics_preserving(self) -> None:
        patcher = ROOT / 'scripts/apply-candidate-b4-beta4-predicate.py'
        self.assertTrue(patcher.exists(), patcher.as_posix())
        source = patcher.read_text(encoding='utf-8')
        required = (
            'p_beta[4]', 'candidate-b4-beta4-predicate-opt', '0x7887fee01585449eULL',
            'kB4ExpectedBytes = 49984', 'kB4ExpectedBound = 2311', 'kB4PredicateCount = 5',
            '1080U, 1353U, 1617U, 1880U, 2143U',
            '1007U, 986U, 1403U, 1667U, 1930U',
            '1082U, 1355U, 1619U, 1882U, 2145U',
            '1081U, 1354U, 1618U, 1881U, 2144U',
            '1277U, 1355U, 1619U, 1882U, 2145U',
            'spv::OpLoad', 'spv::OpCompositeExtract', 'spv::OpUMod', 'spv::OpIEqual',
            'spv::OpLogicalAnd', 'spv::OpSelectionMerge', 'spv::OpBranchConditional',
            'pendingActiveLane', 'selection-branch-adjacency',
            'applyCandidateB4Beta4PredicateCanonicalization(shaderName, spirvBytecode);',
            'logMipmapsSpirvProfile(shaderName, spirvBytecode);',
        )
        for token in required:
            self.assertIn(token, source)
        self.assertLess(source.index('if (opCode == kB4OpSelectionMerge'), source.index('else if (pendingPredicate)'))
        self.assertIn('Emit each direct predicate BEFORE OpSelectionMerge', source)

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
                '#include <cstddef>\n#include <algorithm>\nnamespace {\n'
                '    void logBeta4DependencyProfile(const std::string&, const std::vector<uint8_t>&) {}\n'
                '}\nstruct BindingOffsets {\n    int unused;\n};\n'
                'void translated(const std::string& shaderName, std::vector<uint8_t>& spirvBytecode) {\n'
                '    logMipmapsSpirvProfile(shaderName, spirvBytecode);\n'
                '    logBeta4DependencyProfile(shaderName, spirvBytecode);\n}\n', encoding='utf-8')
            subprocess.run([sys.executable, str(patcher), '--root', str(root)], check=True)
            first = trans.read_text(encoding='utf-8')
            subprocess.run([sys.executable, str(patcher), '--root', str(root)], check=True)
            second = trans.read_text(encoding='utf-8')
        self.assertEqual(first, second)
        self.assertEqual(first.count('bool applyCandidateB4Beta4PredicateCanonicalization('), 1)
        self.assertEqual(first.count('applyCandidateB4Beta4PredicateCanonicalization(shaderName, spirvBytecode);'), 1)
        self.assertLess(first.index('applyCandidateB4Beta4PredicateCanonicalization(shaderName, spirvBytecode);'),
                        first.index('logBeta4DependencyProfile(shaderName, spirvBytecode);'))

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            trans = root / 'src/extract/trans.cpp'
            trans.parent.mkdir(parents=True)
            trans.write_text(
                '#include <cstddef>\n#include <algorithm>\nstruct BindingOffsets {\n    int unused;\n};\n'
                'std::vector<uint8_t> translated(const std::string& shaderName, std::vector<uint8_t> spirvBytecode) {\n'
                '    return spirvBytecode;\n}\n', encoding='utf-8')
            subprocess.run([sys.executable, str(patcher), '--root', str(root)], check=True)
            production = trans.read_text(encoding='utf-8')
        self.assertLess(production.index('applyCandidateB4Beta4PredicateCanonicalization(shaderName, spirvBytecode);'),
                        production.index('return spirvBytecode;'))

        build = (ROOT / 'scripts/build/android.sh').read_text(encoding='utf-8')
        b3 = 'python3 "${REPO_ROOT}/scripts/apply-candidate-b3-beta4-analysis.py" --root "${REPO_ROOT}"'
        b4 = 'python3 "${REPO_ROOT}/scripts/apply-candidate-b4-beta4-predicate.py" --root "${REPO_ROOT}"'
        cleanup = 'python3 "${REPO_ROOT}/scripts/apply-candidate-b-translation-cleanup.py" --root "${REPO_ROOT}"'
        self.assertIn(b4, build)
        self.assertGreater(build.index(b4), build.index(b3))
        self.assertGreater(build.index(b4), build.index(cleanup))
        workflow = (ROOT / '.github/workflows/android-bionic.yml').read_text(encoding='utf-8')
        self.assertIn('python3 tests/android_candidate_b4_beta4_predicate_test.py', workflow)

    def test_b11_pow2_mask_is_exact_guarded_and_falls_back_to_b4(self) -> None:
        patcher = ROOT / 'scripts/apply-candidate-b11-beta4-pow2-mask.py'
        self.assertTrue(patcher.exists(), patcher.as_posix())
        source = patcher.read_text(encoding='utf-8')
        required = (
            'p_beta[4]',
            'candidate-b11-beta4-pow2-mask',
            'spv::OpConstant',
            'spv::OpFunction',
            'spv::OpBitwiseAnd',
            'spv::OpUMod',
            'kB4StepConstants',
            'kB4PredicateCount',
            'value != 0U && (value & (value - 1U)) == 0U',
            'maskValue = value - 1U',
            'uint32_t maskId = 0U',
            'maskId = id',
            'maskValues[predicate] = maskValue',
            'maskConstants[predicate] = maskId',
            'b11SynthesizedMaskCount',
            'b11MaskConstantsInserted',
            'opCode == kB11OpFunction',
            'b4AppendOp3(rewritten, kB11OpConstant, kB4TypeUint',
            'rewritten.at(3) = nextId',
            'applied=0 reason=',
            'applied=1 predicates=',
        )
        for token in required:
            self.assertIn(token, source)

        self.assertNotIn('reason = "mask-constant-missing"', source)
        self.assertLess(source.index('opCode == kB11OpFunction'),
                        source.index('if (opCode == kB4OpSelectionMerge'))

        for forbidden in (
            'spv::OpControlBarrier',
            'spv::OpImageSampleExplicitLod',
            'spv::OpImageWrite',
            'ExecutionModeLocalSize',
            'VK_FORMAT_',
            'supportsTightIcbPacking',
            'LSFGVK_CANDIDATE_B_TIGHT_ICB',
        ):
            self.assertNotIn(forbidden, source)

        build = (ROOT / 'scripts/build/android.sh').read_text(encoding='utf-8')
        b4 = 'python3 "${REPO_ROOT}/scripts/apply-candidate-b4-beta4-predicate.py" --root "${REPO_ROOT}"'
        b11 = 'python3 "${REPO_ROOT}/scripts/apply-candidate-b11-beta4-pow2-mask.py" --root "${REPO_ROOT}"'
        self.assertIn(b11, build)
        self.assertGreater(build.index(b11), build.index(b4))


if __name__ == '__main__':
    unittest.main()
