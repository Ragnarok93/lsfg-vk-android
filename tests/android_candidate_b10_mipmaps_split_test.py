#!/usr/bin/env python3
from __future__ import annotations

import random
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def f32(value: float) -> float:
    return struct.unpack('<f', struct.pack('<f', value))[0]


def reduce4(a: float, b: float, c: float, d: float) -> float:
    # Match the translated shader: ((a+b)+c)+d, then * 0.25, with f32
    # rounding after each operation.
    ab = f32(f32(a) + f32(b))
    abc = f32(ab + f32(c))
    abcd = f32(abc + f32(d))
    return f32(abcd * f32(0.25))


def bits(value: float) -> int:
    return struct.unpack('<I', struct.pack('<f', f32(value)))[0]


def old_pyramid(u1: list[float]) -> list[list[float]]:
    shared = list(u1)
    levels = [[0.0] * n for n in (256, 64, 16, 4, 1)]
    for level, (offset, stride, dim) in enumerate(
        ((1, 2, 16), (2, 4, 8), (4, 8, 4), (8, 16, 2), (16, 32, 1))
    ):
        for x in range(0, 32, stride):
            for y in range(0, 32, stride):
                value = reduce4(
                    shared[x * 32 + y],
                    shared[(x + offset) * 32 + y],
                    shared[x * 32 + y + offset],
                    shared[(x + offset) * 32 + y + offset],
                )
                levels[level][(x // stride) * dim + (y // stride)] = value
                if level != 4:
                    shared[x * 32 + y] = value
    return levels


def compact_pyramid(scratch: list[float]) -> list[list[float]]:
    shared = [0.0] * 256
    levels = [[0.0] * n for n in (256, 64, 16, 4, 1)]
    for x in range(16):
        for y in range(16):
            sx = x * 2
            sy = y * 2
            value = reduce4(
                scratch[sx * 32 + sy],
                scratch[(sx + 1) * 32 + sy],
                scratch[sx * 32 + sy + 1],
                scratch[(sx + 1) * 32 + sy + 1],
            )
            levels[0][x * 16 + y] = value
            shared[x * 16 + y] = value
    for phase, (offset, stride, dim) in enumerate(
        ((1, 2, 8), (2, 4, 4), (4, 8, 2), (8, 16, 1))
    ):
        for x in range(0, 16, stride):
            for y in range(0, 16, stride):
                value = reduce4(
                    shared[x * 16 + y],
                    shared[(x + offset) * 16 + y],
                    shared[x * 16 + y + offset],
                    shared[(x + offset) * 16 + y + offset],
                )
                levels[phase + 1][(x // stride) * dim + (y // stride)] = value
                if phase != 3:
                    shared[x * 16 + y] = value
    return levels


class B10Contract(unittest.TestCase):
    def setUp(self) -> None:
        helper_main = (ROOT / 'framegen/src/adreno_b10_mipmaps.cpp').read_text()
        helper_parts = ''.join(
            part.read_text() for part in sorted(
                (ROOT / 'framegen/src/adreno_b10_mipmaps').glob('part*.inc')))
        self.helper = helper_main + helper_parts
        self.header = (ROOT / 'framegen/include/adreno_b10_mipmaps.hpp').read_text()
        self.shaderpool = (ROOT / 'framegen/src/pool/shaderpool.cpp').read_text()
        self.mipmap = (ROOT / 'framegen/v3.1p_src/shaders/mipmaps.cpp').read_text()
        self.build = (ROOT / 'scripts/build/android.sh').read_text()

    def test_production_fingerprint_matches_existing_b8_baseline(self):
        b8 = (ROOT / 'scripts/apply-candidate-b8-mipmaps-matrix.py').read_text()
        for token in (
            'kExpectedHeadBytes = 28832u',
            'kExpectedHeadBound = 1270u',
            'kExpectedHeadFnv = 0x65d3c6a69e9f9b07ULL',
        ):
            self.assertIn(token, self.helper)
        self.assertIn('kB8ExpectedBytes = 28832', b8)
        self.assertIn('kB8ExpectedBound = 1270', b8)
        self.assertIn('kB8ExpectedFnv = 0x65d3c6a69e9f9b07ULL', b8)

    def test_head_is_fail_closed_and_exact(self):
        for token in (
            'shape.samples == 4 && shape.writes == 10 && shape.barriers == 5',
            'phase0Samples != 4 || phase0Writes.size() != 5',
            'stats.workgroupStoresBeforePhase0 != 1',
            'u1-workgroup-seed-mismatch',
            'scratchMirrorsWorkgroupSeed = true',
            'stats.imageSamplesAfter != 4 || stats.imageWritesAfter != 6',
            'stats.barriersAfter != 0 || stats.workgroupStoresAfter != 0',
            'production-fingerprint',
        ):
            self.assertIn(token, self.helper)
        for forbidden in ('FastMath', 'OpFConvert', 'float16', 'half precision'):
            self.assertNotIn(forbidden, self.helper)

    def test_tail_is_compact_16x16_exact_reduction(self):
        for token in (
            'module.setLocalSize(entry, 16, 16, 1)',
            'module.constu32(256)',
            'CapabilityStorageImageReadWithoutFormat',
            'CapabilityStorageImageWriteWithoutFormat',
            'writeScalarBounded',
            'module.opImageQuerySize',
            'module.opULessThan',
            'const uint32_t u2 = reduce4(',
            'emitReductionPhase(1u, 1u, 2u, 8u, 2u, true)',
            'emitReductionPhase(3u, 2u, 3u, 4u, 4u, true)',
            'emitReductionPhase(7u, 4u, 4u, 2u, 8u, true)',
            'emitReductionPhase(15u, 8u, 5u, 1u, 16u, false)',
        ):
            self.assertIn(token, self.helper)
        # Match the original image-store shape: replicate the scalar to vec4.
        self.assertIn('const std::array<uint32_t, 4> values{x, x, x, x};', self.helper)

    def test_runtime_gate_is_adreno6xx_and_required_storage_features(self):
        self.assertIn('isRuntimeSupported(device)', self.shaderpool)
        self.assertIn('isRuntimeSupported(vk.device)', self.mipmap)
        for token in (
            'isAdreno6xxDevice(properties.vendorID, properties.deviceName)',
            'shaderStorageImageReadWithoutFormat',
            'shaderStorageImageWriteWithoutFormat',
            'VK_FORMAT_R32_SFLOAT',
            'VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT',
        ):
            self.assertIn(token, self.helper)
        self.assertIn('VK_FORMAT_R32_SFLOAT', self.mipmap)

    def test_dispatch_preserves_profiler_anchor_and_includes_tail_cost(self):
        original_barrier = (
            'Utils::BarrierBuilder(buf)\n'
            '        .addW2R((frameCount % 2 == 0) ? this->inImg_0 : this->inImg_1)\n'
            '        .addR2W(this->outImgs)\n'
            '        .build();'
        )
        self.assertIn(original_barrier, self.mipmap)
        self.assertIn('if (this->b10Enabled) {', self.mipmap)
        self.assertIn('.addR2W(this->b10Scratch);', self.mipmap)
        self.assertIn('this->b10TailPipeline.bind(buf);', self.mipmap)
        self.assertIn('return;', self.mipmap)

        wrapper = (ROOT / 'scripts/apply-zero-stage-profile-b10.py').read_text()
        self.assertIn('base.patch_mipmaps_source(root / source_rel, backend)', wrapper)
        self.assertIn('b10-zero-stage-post-head-barrier', wrapper)
        self.assertIn('profilePool->write(buf.handle(), postBarrierQueryIndex)', wrapper)
        self.assertIn('apply-zero-stage-profile-b10.py', self.build)

    def test_b10_diagnostics_preempt_legacy_b8_matrix(self):
        gate = 'if [[ "${LSFGVK_EXPERIMENTAL_B10:-0}" == "1" ]]; then'
        b6 = 'apply-candidate-b6-pipeline-executable-profile.py'
        b10_profile = 'apply-b10-pipeline-executable-prefix.py'
        final = 'elif [[ "${LSFGVK_FINAL_NONADAPTIVE_SWEEP:-0}" == "1" ]]; then'
        self.assertIn(gate, self.build)
        section = self.build[self.build.index('if [[ "${LSFGVK_ZERO_STAGE_PROFILE:-0}"'):]
        self.assertLess(section.index(gate), section.index(final))
        b10_section = section[section.index(gate):section.index(final)]
        self.assertIn(b6, b10_section)
        self.assertIn(b10_profile, b10_section)
        self.assertNotIn('apply-candidate-b8-mipmaps-matrix.py', b10_section)
        self.assertIn('-DLSFGVK_ADRENO_B10_MIPMAPS="${B10_CMAKE}"', self.build)
        self.assertIn('B10 cannot be composed with the legacy B8/final non-adaptive Mipmaps probes', self.build)

    def test_pipeline_profile_prefix_patcher_is_idempotent(self):
        patcher = ROOT / 'scripts/apply-b10-pipeline-executable-prefix.py'
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / 'framegen/src/core/pipeline.cpp'
            path.parent.mkdir(parents=True)
            path.write_text(
                'void log(const std::string& shaderName) {\n'
                '    if (shaderName != "p_mipmaps") return;\n'
                '}\n'
                'void make(const std::string& shaderName) {\n'
                '    const bool captureExecutableInfo = shaderName == "p_mipmaps"\n'
                '        && device.supportsPipelineExecutableProperties();\n'
                '}\n'
            )
            subprocess.run([sys.executable, str(patcher), '--root', td], check=True)
            first = path.read_text()
            subprocess.run([sys.executable, str(patcher), '--root', td], check=True)
            second = path.read_text()
            self.assertEqual(first, second)
            self.assertEqual(first.count('candidate-b10-pipeline-profile shader='), 1)
            self.assertEqual(first.count('shaderName.rfind("p_mipmaps", 0)'), 2)

    def test_compact_reduction_is_bit_exact_for_original_order(self):
        rng = random.Random(0xB10)
        for _ in range(128):
            source = [f32(rng.uniform(-4.0, 4.0)) for _ in range(32 * 32)]
            old = old_pyramid(source)
            compact = compact_pyramid(source)
            for old_level, compact_level in zip(old, compact):
                self.assertEqual([bits(v) for v in old_level], [bits(v) for v in compact_level])

    def test_cross_compiled_selftest_is_wired_only_for_b10(self):
        cmake = (ROOT / 'framegen/CMakeLists.txt').read_text()
        self.assertIn('if(LSFGVK_ADRENO_B10_MIPMAPS)', cmake)
        self.assertIn('lsfg-adreno-b10-mipmaps-test', cmake)
        self.assertIn('LSFGVK_B10_SELF_TEST=1', cmake)


if __name__ == '__main__':
    unittest.main()
