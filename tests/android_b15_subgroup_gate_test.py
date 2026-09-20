#!/usr/bin/env python3
"""Regression coverage for the retained B14 subgroup capability boundary."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "scripts/b14_subgroup_properties.hpp"


class B14SubgroupGateTest(unittest.TestCase):
    def test_b14_gate_is_not_weakened(self):
        text = HEADER.read_text(encoding="utf-8")
        self.assertIn("supportsCooperativeMipmaps", text)
        self.assertIn("return false;", text)
        self.assertIn("Fail closed to the B13 shader", text)
        self.assertNotIn("properties.subgroupSize >= 4U", text)
        self.assertNotIn(
            "VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT",
            text,
        )

    def test_b14_does_not_embed_unpromoted_b15_exact_topology(self):
        text = HEADER.read_text(encoding="utf-8")
        self.assertNotIn("supportsSinglePassMipmaps", text)
        self.assertNotIn("properties.subgroupSize == 128U", text)
        self.assertNotIn("Adreno", text)


if __name__ == "__main__":
    unittest.main()
