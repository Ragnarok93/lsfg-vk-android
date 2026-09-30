#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AdaptiveFlowThermalTest(unittest.TestCase):
    def test_runtime_pressure_accepts_optional_android_thermal_status(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn('key == "thermal_status"', source)
        self.assertIn("pressure.thermalStatusValid", source)
        self.assertIn(".thermalPressureValid =", source)
        # Thermal is optional: the complete pressure record still depends on
        # timestamp/GPU/source/frame-time/slow-ratio, not on thermal presence.
        valid_block = source[source.index("sample.valid ="):source.index("return sample;", source.index("sample.valid ="))]
        self.assertNotIn("thermalStatusValid", valid_block)

    def test_controller_never_uses_thermal_as_standalone_pressure(self) -> None:
        source = (ROOT / "src/adaptive_flow_controller.cpp").read_text(encoding="utf-8")
        pressure = source[source.index("const bool pressure ="):source.index("if (pressure && canLower)")]
        self.assertNotIn("thermalSevere ||", pressure)
        self.assertIn("thermalSevere && pressure", pressure)
        self.assertIn("thermalRecoveryHeadroom", source)


if __name__ == "__main__":
    unittest.main()
