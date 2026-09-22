#!/usr/bin/env python3
"""Regression coverage for the retained B14/B15 capability boundary."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "scripts/b14_subgroup_properties.hpp"


class B14SubgroupGateTest(unittest.TestCase):
    def test_b14_gate_is_exact_profile_scoped_not_broadly_weakened(self):
        text = HEADER.read_text(encoding="utf-8")
        self.assertIn("supportsCooperativeMipmaps", text)
        self.assertIn("properties.subgroupSize == 128U", text)
        self.assertIn(
            "properties.supportedStages == VK_SHADER_STAGE_COMPUTE_BIT",
            text,
        )
        self.assertIn("0x67fU", text)
        self.assertIn("properties.quadOperationsInAllStages == VK_FALSE", text)
        self.assertNotIn("properties.subgroupSize >= 4U", text)
        self.assertNotIn(
            "VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT",
            text,
        )

    def test_b14_does_not_promote_b15_single_pass_topology(self):
        text = HEADER.read_text(encoding="utf-8")
        self.assertNotIn("supportsSinglePassMipmaps", text)
        self.assertNotIn("singlePassMipmaps", text)
        self.assertNotIn("B15", text)
        self.assertNotIn("Adreno", text)
        self.assertNotIn("Turnip", text)
        self.assertNotIn("Xclipse", text)


if __name__ == "__main__":
    unittest.main()
