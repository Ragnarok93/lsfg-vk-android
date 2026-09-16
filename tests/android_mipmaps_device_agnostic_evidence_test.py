#!/usr/bin/env python3
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ANALYZER = ROOT / "scripts/analyze-mipmaps-evidence.py"
COMPAT = ROOT / "scripts/analyze-adreno-mipmaps-evidence.py"
CHECKER = ROOT / "scripts/check-mipmaps-device-agnostic.py"
BUILD = ROOT / "scripts/build/android.sh"


def capture(ms: float, ir_name: str = "GENERIC ISA", ir_line: str = "001: generic-op") -> str:
    return f'''\
lsfg-vk: b12-device-profile vendor_id=0x1234 device_id=0xabcd device_name="Portable GPU" driver_name="Portable Driver" driver_info="1.2.3" api_version=4206592 timestamp_period_ns=12.500 compute_family=2
lsfg-vk: b12-stage-profile mipmaps_samples=120 mipmaps_avg_ms={ms:.3f} beta4_samples=100 beta4_avg_ms=3.000
lsfg-vk: pipeline-exec-property shader=p_mipmaps executable=0 stages=0x20 subgroup_size=32 name="{ir_name}" description="compute"
lsfg-vk: pipeline-exec-ir-line shader=p_mipmaps executable=0 ir=0 line=0 chunk=0 chunks=1 text="{ir_line}"
'''


class DeviceAgnosticMipmapsEvidenceTest(unittest.TestCase):
    def run_analyzer(self, baselines: list[Path], candidate: Path) -> dict:
        cmd = [sys.executable, str(ANALYZER)]
        for baseline in baselines:
            cmd += ["--baseline", str(baseline)]
        cmd += ["--candidate", str(candidate), "--json"]
        result = subprocess.run(cmd, check=True, capture_output=True, text=True)
        return json.loads(result.stdout)

    def test_generic_analyzer_uses_same_device_baseline_variance_without_requiring_ir3(self) -> None:
        self.assertTrue(ANALYZER.exists(), ANALYZER.as_posix())
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            baselines = []
            for index, ms in enumerate((5.20, 5.24, 5.22)):
                path = root / f"baseline-{index}.log"
                path.write_text(capture(ms, ir_line="001: generic-baseline"), encoding="utf-8")
                baselines.append(path)
            candidate = root / "candidate.log"
            candidate.write_text(capture(5.08, ir_line="001: generic-candidate"), encoding="utf-8")
            report = self.run_analyzer(baselines, candidate)

        self.assertEqual(report["candidate"]["device"]["vendor_id"], "0x1234")
        self.assertEqual(report["candidate"]["device"]["device_name"], "Portable GPU")
        self.assertEqual(report["candidate"]["backend"]["name"], "unsupported")
        self.assertFalse(report["candidate"]["backend"]["available"])
        self.assertEqual(report["comparison"]["noise_model"]["baseline_runs"], 3)
        self.assertGreater(report["comparison"]["noise_model"]["stdev_ms"], 0.0)
        self.assertLess(report["comparison"]["mipmaps_delta_percent"], -2.0)
        self.assertEqual(report["comparison"]["verdict"], "promising")
        self.assertEqual(report["comparison"]["risk_flags"], [])

    def test_single_baseline_is_insufficient_for_device_noise_model(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            baseline = root / "baseline.log"
            candidate = root / "candidate.log"
            baseline.write_text(capture(5.22), encoding="utf-8")
            candidate.write_text(capture(5.00, ir_line="001: changed"), encoding="utf-8")
            report = self.run_analyzer([baseline], candidate)
        self.assertEqual(report["comparison"]["verdict"], "insufficient")
        self.assertIn("baseline_noise_model", report["comparison"]["insufficient_reasons"])

    def test_ir3_compatibility_wrapper_keeps_backend_specific_metrics_optional(self) -> None:
        self.assertTrue(COMPAT.exists(), COMPAT.as_posix())
        ir3 = '''\
lsfg-vk: b12-stage-profile mipmaps_samples=120 mipmaps_avg_ms=5.100 beta4_samples=100 beta4_avg_ms=3.000
lsfg-vk: pipeline-exec-property shader=p_mipmaps executable=0 stages=0x20 subgroup_size=64 name="IR3" description="compute"
lsfg-vk: pipeline-exec-stat shader=p_mipmaps executable=0 stat=0 format=uint64 value=8 name="Waves" description="resident waves"
lsfg-vk: pipeline-exec-ir-line shader=p_mipmaps executable=0 ir=0 line=0 chunk=0 chunks=1 text="000: ldl r2.x, r0.x (ss)"
lsfg-vk: pipeline-exec-ir-line shader=p_mipmaps executable=0 ir=0 line=1 chunk=0 chunks=1 text="001: stl r3.x, r2.x (sy)"
'''
        with tempfile.TemporaryDirectory() as temp_dir:
            candidate = Path(temp_dir) / "candidate.log"
            candidate.write_text(ir3, encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(COMPAT), "--candidate", str(candidate), "--json"],
                check=True, capture_output=True, text=True,
            )
            report = json.loads(result.stdout)
        self.assertEqual(report["candidate"]["backend"]["name"], "ir3")
        self.assertTrue(report["candidate"]["backend"]["available"])
        self.assertEqual(report["candidate"]["backend"]["metrics"]["shared_load_count"], 1)
        self.assertEqual(report["candidate"]["backend"]["metrics"]["shared_store_count"], 1)
        self.assertEqual(report["candidate"]["backend"]["metrics"]["resident_waves"], 8)

    def test_candidate_checker_rejects_gpu_identity_branching(self) -> None:
        self.assertTrue(CHECKER.exists(), CHECKER.as_posix())
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            generic = root / "generic.py"
            vendor_specific = root / "vendor.py"
            generic.write_text("def rewrite(words):\n    return words\n", encoding="utf-8")
            vendor_specific.write_text("if vendorID == 0x5143:\n    enable_special_path()\n", encoding="utf-8")
            ok = subprocess.run([sys.executable, str(CHECKER), str(generic)], capture_output=True, text=True)
            bad = subprocess.run([sys.executable, str(CHECKER), str(vendor_specific)], capture_output=True, text=True)
        self.assertEqual(ok.returncode, 0, ok.stderr)
        self.assertNotEqual(bad.returncode, 0)
        self.assertIn("vendorID", bad.stderr)

    def test_android_build_exposes_generic_refinement_interfaces(self) -> None:
        source = BUILD.read_text(encoding="utf-8")
        self.assertIn("LSFGVK_MIPMAPS_EXEC_PROFILE", source)
        self.assertIn("LSFGVK_MIPMAPS_CANDIDATE_SCRIPT", source)
        self.assertIn("check-mipmaps-device-agnostic.py", source)
        self.assertIn("analyze-mipmaps-evidence.py", (ROOT / ".github/workflows/android-bionic.yml").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
