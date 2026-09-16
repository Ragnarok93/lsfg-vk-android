#!/usr/bin/env python3
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ANALYZER = ROOT / "scripts/analyze-adreno-mipmaps-evidence.py"
DEVICE_AGNOSTIC_SUITE = ROOT / "tests/android_mipmaps_device_agnostic_evidence_test.py"

BASELINE = '''\
lsfg-vk: b12-stage-profile mipmaps_samples=120 mipmaps_avg_ms=5.220 beta4_samples=100 beta4_avg_ms=3.060
lsfg-vk: pipeline-exec-property shader=p_mipmaps executable=0 stages=0x20 subgroup_size=64 name="IR3" description="compute"
lsfg-vk: pipeline-exec-stat shader=p_mipmaps executable=0 stat=0 format=uint64 value=8 name="Waves" description="resident waves"
lsfg-vk: pipeline-exec-ir-line shader=p_mipmaps executable=0 ir=0 line=0 chunk=0 chunks=1 text="000: nop"
lsfg-vk: pipeline-exec-ir-line shader=p_mipmaps executable=0 ir=0 line=1 chunk=0 chunks=1 text="001: ldl r2.x, r0.x (ss)"
lsfg-vk: pipeline-exec-ir-line shader=p_mipmaps executable=0 ir=0 line=2 chunk=0 chunks=1 text="002: stl r3.x, r2.x (sy)"
'''

CANDIDATE = '''\
lsfg-vk: b12-stage-profile mipmaps_samples=120 mipmaps_avg_ms=5.070 beta4_samples=108 beta4_avg_ms=3.010
lsfg-vk: pipeline-exec-property shader=p_mipmaps executable=0 stages=0x20 subgroup_size=64 name="IR3" description="compute"
lsfg-vk: pipeline-exec-stat shader=p_mipmaps executable=0 stat=0 format=uint64 value=8 name="Waves" description="resident waves"
lsfg-vk: pipeline-exec-ir-line shader=p_mipmaps executable=0 ir=0 line=0 chunk=0 chunks=1 text="000: ldl r2.x, r0.x (ss)"
lsfg-vk: pipeline-exec-ir-line shader=p_mipmaps executable=0 ir=0 line=1 chunk=0 chunks=1 text="001: stl r3.x, r2.x (sy)"
'''


class MipmapsEvidenceAnalyzerContractTest(unittest.TestCase):
    def test_analyzer_combines_weighted_timing_ir_and_candidate_gate(self) -> None:
        self.assertTrue(ANALYZER.exists(), ANALYZER.as_posix())
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            baseline = root / "baseline.log"
            candidate = root / "candidate.log"
            baseline.write_text(BASELINE, encoding="utf-8")
            candidate.write_text(CANDIDATE, encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(ANALYZER), "--baseline", str(baseline), "--candidate", str(candidate), "--json"],
                check=True, capture_output=True, text=True,
            )
            report = json.loads(result.stdout)

        self.assertAlmostEqual(report["baseline"]["timing"]["mipmaps_avg_ms"], 5.22, places=3)
        self.assertAlmostEqual(report["candidate"]["timing"]["mipmaps_avg_ms"], 5.07, places=3)
        self.assertAlmostEqual(report["comparison"]["mipmaps_delta_ms"], -0.15, places=3)
        self.assertEqual(report["baseline"]["ir"]["nop_count"], 1)
        self.assertEqual(report["candidate"]["ir"]["nop_count"], 0)
        self.assertEqual(report["candidate"]["ir"]["shared_load_count"], 1)
        self.assertEqual(report["candidate"]["ir"]["shared_store_count"], 1)
        self.assertEqual(report["candidate"]["ir"]["ss_count"], 1)
        self.assertEqual(report["candidate"]["ir"]["sy_count"], 1)
        self.assertEqual(report["comparison"]["verdict"], "promising")
        self.assertEqual(report["comparison"]["risk_flags"], [])

    def test_analyzer_flags_spill_or_wave_regression(self) -> None:
        bad = CANDIDATE.replace('value=8 name="Waves"', 'value=7 name="Waves"') + (
            'lsfg-vk: pipeline-exec-ir-line shader=p_mipmaps executable=0 ir=0 '
            'line=2 chunk=0 chunks=1 text="002: spill r9.x"\n'
        )
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            baseline = root / "baseline.log"
            candidate = root / "candidate.log"
            baseline.write_text(BASELINE, encoding="utf-8")
            candidate.write_text(bad, encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(ANALYZER), "--baseline", str(baseline), "--candidate", str(candidate), "--json"],
                check=True, capture_output=True, text=True,
            )
            report = json.loads(result.stdout)

        self.assertIn("spill_markers_increased", report["comparison"]["risk_flags"])
        self.assertIn("resident_waves_decreased", report["comparison"]["risk_flags"])
        self.assertEqual(report["comparison"]["verdict"], "reject")

    def test_device_agnostic_contract_suite(self) -> None:
        result = subprocess.run(
            [sys.executable, str(DEVICE_AGNOSTIC_SUITE)],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
