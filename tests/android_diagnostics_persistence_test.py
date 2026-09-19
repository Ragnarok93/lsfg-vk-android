#!/usr/bin/env python3
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AndroidDiagnosticsPersistenceTest(unittest.TestCase):
    def test_native_metrics_are_persisted_only_from_completed_window(self) -> None:
        header = (ROOT / "include/android_diagnostics.hpp").read_text(encoding="utf-8")
        diagnostics = (ROOT / "src/android_diagnostics.cpp").read_text(encoding="utf-8")
        context = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("appendRuntimeLine", header)
        self.assertIn('"diagnostics.log"', diagnostics)
        self.assertIn("kMaxRuntimeLogBytes", diagnostics)
        self.assertIn("kFlushEveryLines = 4", diagnostics)
        self.assertIn("timestamp_ms=", diagnostics)

        self.assertEqual(context.count("AndroidDiagnostics::appendRuntimeLine"), 1)
        window = context.index("if (elapsedSeconds >= 1.0)")
        call = context.index("AndroidDiagnostics::appendRuntimeLine")
        reset = context.index("metrics.windowStart = cycleEnd", call)
        self.assertLess(window, call)
        self.assertLess(call, reset)
        self.assertIn("std::ostringstream metricsLine", context[window:call])
        self.assertIn("lsfg-vk: metrics", context[window:call])


if __name__ == "__main__":
    unittest.main()
