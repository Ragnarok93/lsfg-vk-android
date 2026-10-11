#!/usr/bin/env python3
"""Production-controller behavioral probes and long-lived unchanged scene holds."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
CASES = (
    'unprofitable_target', 'wsi_contribution', 'known_tiny_flow',
    'unknown_output_recovery', 'history_provenance', 'rejected_probe_hold',
    'rejected_recovery_hold', 'cadence_without_gpu', 'measured_benefit',
)

class AdaptiveFlowStabilityTest(unittest.TestCase):
    def test_controller_behavior(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = str(pathlib.Path(directory) / 'flow-audit')
            subprocess.run([
                'c++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                '-I', str(ROOT / 'include'),
                str(ROOT / 'src/adaptive_flow_controller.cpp'),
                str(ROOT / 'tests/adaptive_flow_stability_audit_test.cpp'),
                '-o', binary,
            ], check=True)
            for case in CASES:
                with self.subTest(case=case):
                    subprocess.run([binary, case], check=True)

if __name__ == '__main__':
    unittest.main()
