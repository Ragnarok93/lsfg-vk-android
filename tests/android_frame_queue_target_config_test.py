#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

class AndroidFrameQueueTargetConfigTest(unittest.TestCase):
    def test_queue_target_is_config_and_telemetry_only(self) -> None:
        config = (ROOT / "include/config/config.hpp").read_text(encoding="utf-8")
        parser = (ROOT / "src/config/config.cpp").read_text(encoding="utf-8")
        context = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("frameQueueEnabled", config)
        self.assertIn("frameQueueTarget", config)
        self.assertIn('"frame_queue_enabled"', parser)
        self.assertIn('"frame_queue_target"', parser)
        self.assertIn("frame_queue_enabled=", context)
        self.assertIn("frame_queue_target=", context)

        # The Vulkan layer may report the host queue preference, but it must not
        # use it to alter interpolation density or the protected sync topology.
        present_start = context.index("VkResult LsContext::present(")
        present = context[present_start:]
        self.assertNotIn("conf.frameQueueTarget", present)
        self.assertNotIn("conf.frameQueueEnabled", present)

    def test_queue_target_is_bounded_to_eden_compatible_range(self) -> None:
        parser = (ROOT / "src/config/config.cpp").read_text(encoding="utf-8")
        self.assertIn("Frame queue target must be between 0 and 2", parser)

if __name__ == "__main__":
    unittest.main()
