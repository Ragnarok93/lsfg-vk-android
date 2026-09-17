#!/usr/bin/env python3
"""Regression coverage for the capability split between B14 and B15."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "scripts/b14_subgroup_properties.hpp"

class B15SubgroupGateTest(unittest.TestCase):
    def test_b14_gate_is_not_weakened(self):
        text = HEADER.read_text(encoding="utf-8")
        self.assertIn("supportsCooperativeMipmaps", text)
        self.assertIn("properties.subgroupSize >= 4U", text)
        self.assertIn("VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT", text)

    def test_b15_requires_exact_adreno_topology(self):
        text = HEADER.read_text(encoding="utf-8")
        self.assertIn("supportsSinglePassMipmaps", text)
        self.assertIn("properties.subgroupSize == 128U", text)
        self.assertIn("requiredStages", text)
        self.assertIn("requiredOperations", text)

if __name__ == "__main__":
    unittest.main()
