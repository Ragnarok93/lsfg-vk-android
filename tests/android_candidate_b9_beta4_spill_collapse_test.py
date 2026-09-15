#!/usr/bin/env python3
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PARTS = ROOT / "scripts/b9_beta4_spill_collapse"


def b9_source() -> str:
    patcher = (ROOT / "scripts/apply-candidate-b9-beta4-spill-collapse.py").read_text()
    helper = "".join(
        (PARTS / name).read_text()
        for name in ("part1.inc", "part2.inc", "part3.inc")
    )
    return patcher + helper


class B9Contract(unittest.TestCase):
    def test_b9_targets_unique_hot_helper_not_entry_wrapper(self):
        source = b9_source()
        for token in (
            "b9FindBeta4HotFunction",
            "B9FunctionShape",
            "hotFunctions == 1U",
            "shape.samples == 18U",
            "shape.writes == 6U",
            "shape.barriers == 5U",
            "shape.phase0Samples == 18U",
            "shape.predicates == kB9PredicateConditions.size()",
            "inTargetFunction",
            "targetFunctionId",
            'reason=hot-function',
            'entry_function=',
            'target_function=',
        ):
            self.assertIn(token, source)
        self.assertNotIn("inEntryFunction", source)

    def test_b9_remains_exact_and_fail_closed(self):
        source = b9_source()
        for token in (
            "kB9ExpectedBytes = 50764U",
            "kB9ExpectedBound = 2351U",
            "samples == 18U",
            "writes == 6U",
            "barriers == 5U",
            "local32 == 1U",
            "b9SameInventory",
            "candidateShape.samples != targetShape.samples",
            "candidateShape.writes != targetShape.writes",
            "candidateShape.barriers != targetShape.barriers",
            "candidateShape.phase0Samples != targetShape.phase0Samples",
            "candidateShape.predicates != targetShape.predicates",
        ):
            self.assertIn(token, source)
        for forbidden in ("FastMath", "OpFConvert", "ExecutionModeLocalSize = 16"):
            self.assertNotIn(forbidden, source)

    def test_b9_is_experimental_opt_in_after_b4(self):
        build = (ROOT / "scripts/build/android.sh").read_text()
        b4 = 'python3 "${REPO_ROOT}/scripts/apply-candidate-b4-beta4-predicate.py" --root "${REPO_ROOT}"'
        b9 = 'python3 "${REPO_ROOT}/scripts/apply-candidate-b9-beta4-spill-collapse.py" --root "${REPO_ROOT}"'
        gate = 'if [[ "${LSFGVK_EXPERIMENTAL_B9:-0}" == "1" ]]; then'
        self.assertIn(b4, build)
        self.assertIn(gate, build)
        self.assertIn(b9, build)
        self.assertLess(build.index(b4), build.index(gate))
        self.assertLess(build.index(gate), build.index(b9))
        self.assertLess(build.index(b9), build.index("\nfi\n", build.index(b9)))

    def test_b9_patcher_is_idempotent_and_chains_after_b4(self):
        patcher = ROOT / "scripts/apply-candidate-b9-beta4-spill-collapse.py"
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
            first = tr.read_text()
            subprocess.run([sys.executable, str(patcher), "--root", td], check=True)
            second = tr.read_text()
            self.assertEqual(first, second)
            self.assertEqual(first.count("bool applyCandidateB9Beta4SpillCollapse("), 1)
            self.assertEqual(
                first.count("applyCandidateB9Beta4SpillCollapse(shaderName, spirvBytecode);"), 1
            )
            self.assertLess(
                first.index("applyCandidateB4Beta4PredicateCanonicalization(shaderName, spirvBytecode);"),
                first.index("applyCandidateB9Beta4SpillCollapse(shaderName, spirvBytecode);"),
            )

    def test_full_apply_selftest_selects_helper_and_preserves_contract(self):
        helper = "".join(
            (PARTS / name).read_text()
            for name in ("part1.inc", "part2.inc", "part3.inc")
        )
        prefix = r'''
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
namespace spv {
constexpr uint16_t OpImageSampleExplicitLod = 88U;
constexpr uint16_t OpImageWrite = 99U;
constexpr uint16_t OpControlBarrier = 224U;
}
'''
        harness = r'''
static void emit(std::vector<uint32_t>& words, uint16_t opcode,
        std::initializer_list<uint32_t> operands) {
    words.push_back((static_cast<uint32_t>(operands.size() + 1U) << 16U) | opcode);
    words.insert(words.end(), operands.begin(), operands.end());
}

static void appendWrapper(std::vector<uint32_t>& words, uint32_t id) {
    emit(words, 54U, {1U, id, 0U, 1U});
    emit(words, 248U, {id + 1000U});
    emit(words, 253U, {});
    emit(words, 56U, {});
}

static void appendHot(std::vector<uint32_t>& words, uint32_t id, uint32_t resultBase) {
    emit(words, 54U, {1U, id, 0U, 1U});
    emit(words, 248U, {id + 1000U});
    for (uint32_t i = 0U; i < 18U; ++i)
        emit(words, 88U, {10U, resultBase + i, 20U, 21U, 2U, 22U});
    for (uint32_t i = 0U; i < 18U; ++i)
        emit(words, 129U, {10U, resultBase + 100U + i, resultBase + i, 23U});
    emit(words, 224U, {1U, 1U, 1U});
    for (uint32_t i = 0U; i < 6U; ++i)
        emit(words, 99U, {30U + i, 31U, 32U});
    for (uint32_t i = 0U; i < 4U; ++i)
        emit(words, 224U, {1U, 1U, 1U});
    for (size_t i = 0; i < kB9MergeLabels.size(); ++i) {
        emit(words, 247U, {kB9MergeLabels[i], 0U});
        emit(words, 250U, {kB9PredicateConditions[i], 40U + static_cast<uint32_t>(i),
            50U + static_cast<uint32_t>(i)});
    }
    emit(words, 253U, {});
    emit(words, 56U, {});
}

static std::vector<uint8_t> makeModule(bool secondHot) {
    std::vector<uint32_t> words{
        kB9SpirvMagic, 0x00010000U, 0U, kB9ExpectedBound, 0U
    };
    emit(words, 15U, {5U, 2U});
    emit(words, 16U, {2U, kB9ExecutionModeLocalSize, 32U, 32U, 1U});
    appendWrapper(words, 2U);
    appendHot(words, 4U, 100U);
    if (secondHot)
        appendHot(words, 6U, 400U);
    if (!secondHot) {
        const size_t targetWords = kB9ExpectedBytes / sizeof(uint32_t);
        if (words.size() > targetWords)
            std::abort();
        while (words.size() < targetWords)
            emit(words, 0U, {});
    }
    std::vector<uint8_t> bytes(words.size() * sizeof(uint32_t));
    std::memcpy(bytes.data(), words.data(), bytes.size());
    return bytes;
}

int main() {
    auto bytes = makeModule(false);
    std::vector<B9Instruction> before;
    if (!b9Parse(bytes, before) || !b9Beta4Baseline(bytes, before))
        return 10;
    uint32_t entry = 0U;
    uint32_t target = 0U;
    B9FunctionShape shape{};
    if (!b9FindBeta4HotFunction(bytes, before, entry, target, shape))
        return 11;
    if (entry != 2U || target != 4U || shape.samples != 18U
            || shape.writes != 6U || shape.barriers != 5U
            || shape.phase0Samples != 18U || shape.predicates != 5U)
        return 12;

    if (!applyCandidateB9Beta4SpillCollapse("p_beta[4]", bytes))
        return 13;
    std::vector<B9Instruction> after;
    if (!b9Parse(bytes, after) || !b9Beta4Baseline(bytes, after))
        return 14;
    std::vector<uint32_t> sampleIds;
    for (const auto& instruction : after) {
        if (instruction.opCode == kB9OpImageSampleExplicitLod)
            sampleIds.push_back(b9ReadWord(bytes, instruction.word + 2U));
    }
    if (sampleIds.size() != 18U)
        return 15;
    for (uint32_t i = 0U; i < 18U; ++i) {
        if (sampleIds[i] != 100U + i)
            return 16;
    }

    auto ambiguous = makeModule(true);
    std::vector<B9Instruction> ambiguousInstructions;
    if (!b9Parse(ambiguous, ambiguousInstructions))
        return 17;
    entry = target = 0U;
    shape = {};
    if (b9FindBeta4HotFunction(
            ambiguous, ambiguousInstructions, entry, target, shape))
        return 18;

    std::cout << "B9_SELFTEST entry=2 target=4 samples=18 writes=6 barriers=5 "
                 "phase0_samples=18 predicates=5 sample_order=preserved ambiguous=rejected\n";
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as td:
            source = Path(td) / "b9_selftest.cpp"
            binary = Path(td) / "b9_selftest"
            source.write_text(prefix + helper + harness)
            compile_result = subprocess.run(
                ["g++", "-std=c++17", "-Wall", "-Wextra", str(source), "-o", str(binary)],
                text=True,
                capture_output=True,
            )
            self.assertEqual(compile_result.returncode, 0, compile_result.stderr)
            run = subprocess.run([str(binary)], text=True, capture_output=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("B9_SELFTEST entry=2 target=4", run.stdout)
            for token in (
                "applied=1",
                "entry_function=2",
                "target_function=4",
                "hot_samples=18",
                "hot_writes=6",
                "hot_barriers=5",
                "phase0_samples=18",
                "predicates=5",
                "streamed_samples=18",
            ):
                self.assertIn(token, run.stderr)


if __name__ == "__main__":
    unittest.main()
