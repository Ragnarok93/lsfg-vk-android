#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "scripts/b14_mipmaps_tail_fusion.hpp"
SUBGROUP_HEADER = ROOT / "scripts/b14_subgroup_properties.hpp"
SUBGROUP_TEST = ROOT / "tests/android_b14_subgroup_property_query_test.py"
PATCHER = ROOT / "scripts/apply-candidate-b14-mipmaps-tail-fusion.py"
CHECKER = ROOT / "scripts/check-mipmaps-device-agnostic.py"
FIXTURE = ROOT / "tests/fixtures/p_mipmaps_b13.spv"
FIXTURE_SHA256 = "68c68ffd7308d0cc742aa3e9ecbd00f44c92893c23df5cd62e1318cdb75b9046"


def f32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


def add(a: float, b: float) -> float:
    return f32(f32(a) + f32(b))


def quarter(value: float) -> float:
    return f32(f32(value) * f32(0.25))


def baseline_tail(values: list[float]) -> tuple[list[float], float]:
    mip5 = []
    for a, b, c, d in ((0, 1, 4, 5), (2, 3, 6, 7),
                       (8, 9, 12, 13), (10, 11, 14, 15)):
        reduced = add(values[b], values[a])
        reduced = add(values[c], reduced)
        reduced = add(values[d], reduced)
        mip5.append(quarter(reduced))
    final = add(mip5[1], mip5[0])
    final = add(mip5[2], final)
    final = add(mip5[3], final)
    return mip5, quarter(final)


def fused_tail(values: list[float]) -> tuple[list[float], float]:
    # Four logical lanes own one 2x2 quadrant each. The final reduction uses
    # the same FP32 order as the original workgroup-barrier tail.
    grid = [values[row * 4:(row + 1) * 4] for row in range(4)]
    out: list[float] = []
    for y in (0, 2):
        for x in (0, 2):
            reduced = add(grid[y][x + 1], grid[y][x])
            reduced = add(grid[y + 1][x], reduced)
            reduced = add(grid[y + 1][x + 1], reduced)
            out.append(quarter(reduced))
    final = add(out[1], out[0])
    final = add(out[2], final)
    final = add(out[3], final)
    return out, quarter(final)


def instructions(code: bytes) -> list[tuple[int, list[int]]]:
    words = struct.unpack(f"<{len(code) // 4}I", code)
    parsed: list[tuple[int, list[int]]] = []
    cursor = 5
    while cursor < len(words):
        first = words[cursor]
        word_count = first >> 16
        opcode = first & 0xFFFF
        if word_count == 0 or cursor + word_count > len(words):
            raise AssertionError(f"invalid SPIR-V instruction at word {cursor}")
        parsed.append((opcode, list(words[cursor + 1:cursor + word_count])))
        cursor += word_count
    return parsed


def shared_access_counts(code: bytes) -> tuple[int, int]:
    # The captured module's sole Workgroup variable is id 39. Track pointers
    # derived from it independently of the production rewriter.
    shared_pointers = {39}
    loads = 0
    stores = 0
    for opcode, operands in instructions(code):
        if opcode in (65, 66) and len(operands) >= 3 and operands[2] in shared_pointers:
            shared_pointers.add(operands[1])
        elif opcode == 61 and len(operands) >= 3 and operands[2] in shared_pointers:
            loads += 1
        elif opcode == 62 and operands and operands[0] in shared_pointers:
            stores += 1
    return loads, stores


HARNESS = r'''
#include "b14_mipmaps_tail_fusion.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

static std::vector<uint8_t> readFile(const char* path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>()};
}

static void writeFile(const char* path, const std::vector<uint8_t>& bytes) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

int main(int argc, char** argv) {
    if (argc != 3) return 90;
    auto code = readFile(argv[1]);
    const auto original = code;
    auto first = b14::fuseTail(code);
    if (!first.applied || first.alreadyApplied) {
        std::cerr << "first rewrite rejected: " << first.reason
                  << " barriers=" << first.barriersBefore
                  << " image_writes=" << first.imageWritesBefore << '\n';
        return 10;
    }
    if (first.barriersBefore != 5 || first.barriersAfter != 4) return 11;
    if (first.tailDynamicLoadsBefore != 15 || first.tailDynamicLoadsAfter != 12) return 12;
    if (first.tailWorkgroupStoresBefore != 4 || first.tailWorkgroupStoresAfter != 0) return 13;
    if (first.imageWritesBefore != 10 || first.imageWritesAfter != 10) return 14;
    if (first.tailCriticalPathLoadsBefore != 6 || first.tailCriticalPathLoadsAfter != 3) return 15;
    if (first.tailParallelLanesAfter != 4 || first.subgroupBroadcastsAfter != 4) return 16;
    if (first.staticWorkgroupLoadsBefore != 15 || first.staticWorkgroupLoadsAfter != 12) return 17;
    writeFile(argv[2], code);

    const auto once = code;
    auto second = b14::fuseTail(code);
    if (second.applied || !second.alreadyApplied || code != once) return 20;

    auto damagedCandidate = once;
    damagedCandidate.at(24) ^= 1U;
    const auto damagedCandidateBefore = damagedCandidate;
    auto damagedCandidateReport = b14::fuseTail(damagedCandidate);
    if (damagedCandidateReport.applied || damagedCandidateReport.alreadyApplied
            || damagedCandidate != damagedCandidateBefore) return 25;

    auto mutated = original;
    mutated.at(24) ^= 1U;
    const auto mutatedBefore = mutated;
    auto rejected = b14::fuseTail(mutated);
    if (rejected.applied || rejected.alreadyApplied || mutated != mutatedBefore) return 30;

    std::cout << first.marker << '\n';
    return 0;
}
'''


class AndroidCandidateB14MipmapsTailFusionTest(unittest.TestCase):
    def test_tail_fusion_preserves_exact_fp32_reduction_order(self) -> None:
        cases = [
            [0.0] * 16,
            [1.0] * 16,
            [float(i) / 17.0 for i in range(16)],
            [(-1.0 if i % 2 else 1.0) * (2.0 ** (i - 8)) for i in range(16)],
        ]
        rng = random.Random(0xB14)
        for _ in range(4096):
            cases.append([f32(rng.uniform(-65504.0, 65504.0)) for _ in range(16)])
        for values in cases:
            self.assertEqual(baseline_tail(values), fused_tail(values))

    def test_real_rewriter_is_fail_closed_idempotent_and_structurally_exact(self) -> None:
        self.assertEqual(hashlib.sha256(FIXTURE.read_bytes()).hexdigest(), FIXTURE_SHA256)
        self.assertTrue(HEADER.exists(), HEADER.as_posix())
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as tmp:
            temp = Path(tmp)
            harness = temp / "b14_harness.cpp"
            executable = temp / "b14_harness"
            output = temp / "p_mipmaps_b14.spv"
            harness.write_text(HARNESS, encoding="utf-8")
            subprocess.run(
                [compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                 "-I", str(ROOT / "scripts"), "-I", str(ROOT / "thirdparty/dxbc/include/spirv"),
                 str(harness), "-o", str(executable)],
                check=True,
            )
            result = subprocess.run(
                [str(executable), str(FIXTURE), str(output)],
                capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            transformed_bytes = output.read_bytes()
            validator = os.environ.get("SPIRV_VAL") or shutil.which("spirv-val")
            if validator:
                validated = subprocess.run(
                    [validator, "--target-env", "vulkan1.3", str(output)],
                    capture_output=True, text=True,
                )
                self.assertEqual(validated.returncode, 0, validated.stderr)
        self.assertIn("candidate-b14-mipmaps-tail-fusion", result.stdout)
        self.assertNotEqual(transformed_bytes, FIXTURE.read_bytes())
        baseline_ops = [opcode for opcode, _ in instructions(FIXTURE.read_bytes())]
        candidate_ops = [opcode for opcode, _ in instructions(transformed_bytes)]
        self.assertEqual((baseline_ops.count(224), candidate_ops.count(224)), (5, 4))
        self.assertEqual((baseline_ops.count(99), candidate_ops.count(99)), (10, 10))
        self.assertEqual(candidate_ops.count(337), 4)
        self.assertEqual(shared_access_counts(FIXTURE.read_bytes()), (15, 5))
        self.assertEqual(shared_access_counts(transformed_bytes), (12, 4))

    def test_candidate_wiring_is_idempotent_and_device_agnostic(self) -> None:
        self.assertTrue(PATCHER.exists(), PATCHER.as_posix())
        subgroup_result = subprocess.run(
            [sys.executable, str(SUBGROUP_TEST)], capture_output=True, text=True,
        )
        self.assertEqual(subgroup_result.returncode, 0, subgroup_result.stderr)
        for candidate in (PATCHER, HEADER, SUBGROUP_HEADER):
            result = subprocess.run(
                [sys.executable, str(CHECKER), str(candidate)],
                capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)

        patcher_text = PATCHER.read_text(encoding="utf-8")
        subgroup_text = SUBGROUP_HEADER.read_text(encoding="utf-8")
        self.assertIn("VK_SUBGROUP_FEATURE_BALLOT_BIT", subgroup_text)
        self.assertIn("mipmapsSubgroupBroadcastSupported", patcher_text)
        self.assertIn("enableCooperativeMipmaps", patcher_text)
        self.assertIn("fallback=b13", patcher_text)
        self.assertIn("gamenative-helper-process-override-guard", patcher_text)

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            translation = root / "src/extract/trans.cpp"
            translation.parent.mkdir(parents=True)
            translation.write_text(
                '#include "extract/trans.hpp"\n\n'
                '#include <vector>\n\n'
                'using namespace Extract;\n\n'
                'std::vector<uint8_t> translated(const std::string& shaderName, '
                'std::vector<uint8_t> spirvBytecode) {\n'
                '    return spirvBytecode;\n'
                '}\n',
                encoding="utf-8",
            )
            process_source = root / "src/utils/utils.cpp"
            process_source.parent.mkdir(parents=True, exist_ok=True)
            process_source.write_text(
                r'''std::pair<std::string, std::string> Utils::getProcessName() {
    // GameNative / Wine-on-Android: /proc/self/exe points at the Wine loader,
    // not the game .exe. Accept an explicit override from the launcher so
    // per-game matching in the TOML still works.
    const char* process_exe = std::getenv("LSFG_PROCESS_EXE");
    if (process_exe && *process_exe != '\0')
        return { process_exe, process_exe };
}
''',
                encoding="utf-8",
            )
            for _ in range(2):
                subprocess.run(
                    [sys.executable, str(PATCHER), "--root", str(root)],
                    check=True,
                )
            transformed = translation.read_text(encoding="utf-8")
            transformed_process = process_source.read_text(encoding="utf-8")

        self.assertEqual(transformed.count("b14_mipmaps_tail_fusion.hpp"), 1)
        self.assertEqual(transformed.count("b14::fuseTail(spirvBytecode)"), 1)
        self.assertIn('shaderName == "p_mipmaps"', transformed)
        self.assertIn("candidate-b14-mipmaps-tail-fusion", transformed)
        self.assertIn("gamenative-helper-process-override-guard", transformed_process)
        self.assertIn("*process_exe != '\\0'", transformed_process)
        self.assertIn("cmdline.at(cmdline_len) = '\\0';", transformed_process)
        self.assertIn("<< \" configured_target=\" << process_exe << '\\n';", transformed_process)
        self.assertNotIn("\x00", transformed_process)

        build = (ROOT / "scripts/build/android.sh").read_text(encoding="utf-8")
        self.assertNotIn("LSFGVK_MIPMAPS_CANDIDATE_SCRIPT", build)
        self.assertIn('B14_SCRIPT="${REPO_ROOT}/scripts/apply-candidate-b14-mipmaps-tail-fusion.py"', build)
        self.assertIn("check-mipmaps-device-agnostic.py", build)


if __name__ == "__main__":
    unittest.main()
