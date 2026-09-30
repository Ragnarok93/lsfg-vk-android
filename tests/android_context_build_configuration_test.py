#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class ContextBuildConfigurationTest(unittest.TestCase):
    def test_final_context_configuration_is_logged_before_context_construction(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        log = source.index('lsfg-vk: framegen-context-create')
        create = min(
            source.index("LSFG_3_1::createAdaptiveContextFromAHB", log),
            source.index("LSFG_3_1P::createAdaptiveContextFromAHB", log),
        )
        self.assertLess(log, create)
        for marker in (
            "generation_capacity=",
            "flow_states=",
            "transport=",
            "vulkan_path=",
            "sync_path=",
            "shader_target=0x",
            "duplicate_effective=",
        ):
            self.assertIn(marker, source)

    def test_duplicate_effective_context_rebuilds_are_visible(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn("lastFramegenContextBuildSignature", source)
        self.assertIn("std::chrono::seconds(2)", source)
        self.assertIn("framegen-context-rebuild-warning", source)


if __name__ == "__main__":
    unittest.main()
