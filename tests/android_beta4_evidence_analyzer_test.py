#!/usr/bin/env python3
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ANALYZER = ROOT / "scripts/analyze-beta4-evidence.py"


def evidence(beta_ms: float, instructions: tuple[str, ...], waves: int = 8) -> str:
    records = [
        "lsfg-vk: b12-stage-profile mipmaps_samples=80 mipmaps_avg_ms=5.2 "
        f"beta4_samples=80 beta4_avg_ms={beta_ms}",
        'lsfg-vk: b12-device-profile vendor_id=0x5143 device_id=0x650 '
        'device_name="test" driver_version=1 api_version=1 timestamp_period_ns=1 compute_family=0',
        'lsfg-vk: pipeline-exec-property shader=p_beta[4] executable=0 stages=0x20 '
        'subgroup_size=64 name="IR3" description="compute"',
        'lsfg-vk: pipeline-exec-stat shader=p_beta[4] executable=0 stat=0 '
        f'format=uint64 value={waves} name="Waves" description="resident waves"',
    ]
    records.extend(
        "lsfg-vk: pipeline-exec-ir-line shader=p_beta[4] executable=0 ir=0 "
        f'line={index} chunk=0 chunks=1 text="{instruction}"'
        for index, instruction in enumerate(instructions)
    )
    return "\n".join(records) + "\n"


class Beta4EvidenceAnalyzerTest(unittest.TestCase):
    def test_compares_repeated_timing_and_final_executable(self) -> None:
        self.assertTrue(ANALYZER.exists(), ANALYZER.as_posix())
        baseline_a = evidence(3.00, ("000: nop", "001: and.u r2.x, r0.x, r1.x (ss)"))
        baseline_b = evidence(3.04, ("000: nop", "001: and.u r2.x, r0.x, r1.x (ss)"))
        candidate = evidence(2.88, ("000: or.b r1.x, r0.x, r0.y", "001: and.u r2.x, r1.x, r3.x"))

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            paths = [root / name for name in ("base-a.log", "base-b.log", "candidate.log")]
            for path, text in zip(paths, (baseline_a, baseline_b, candidate), strict=True):
                path.write_text(text, encoding="utf-8")
            result = subprocess.run(
                [
                    sys.executable,
                    str(ANALYZER),
                    "--baseline", str(paths[0]),
                    "--baseline", str(paths[1]),
                    "--candidate", str(paths[2]),
                    "--json",
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            report = json.loads(result.stdout)

        self.assertEqual(report["candidate"]["shader"], "p_beta[4]")
        self.assertEqual(report["candidate"]["backend"]["name"], "ir3")
        self.assertEqual(report["candidate"]["backend"]["metrics"]["instruction_lines"], 2)
        self.assertTrue(report["comparison"]["executable_changed"])
        self.assertLess(report["comparison"]["beta4_delta_ms"], -0.1)
        self.assertEqual(report["comparison"]["verdict"], "promising")

    def test_rejects_wave_or_spill_regression(self) -> None:
        baseline = evidence(3.0, ("000: and.u r2.x, r0.x, r1.x",))
        candidate = evidence(2.8, ("000: spill r9.x",), waves=7)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            base = root / "base.log"
            cand = root / "candidate.log"
            base.write_text(baseline, encoding="utf-8")
            cand.write_text(candidate, encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(ANALYZER), "--baseline", str(base),
                 "--baseline", str(base), "--candidate", str(cand), "--json"],
                check=True, capture_output=True, text=True,
            )
            report = json.loads(result.stdout)

        self.assertIn("spill_markers_increased", report["comparison"]["risk_flags"])
        self.assertIn("resident_waves_decreased", report["comparison"]["risk_flags"])
        self.assertEqual(report["comparison"]["verdict"], "reject")


if __name__ == "__main__":
    unittest.main()
