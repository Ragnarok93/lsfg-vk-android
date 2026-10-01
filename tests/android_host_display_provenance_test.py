#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidHostDisplayProvenanceTest(unittest.TestCase):
    def test_every_present_has_a_universal_delivery_identity(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("hostDeliveryId_", header)
        self.assertIn("publishHostFrameProvenance", source)
        self.assertIn("LSFG_FRAME_PROVENANCE", source)
        self.assertIn("delivery_id=", source)
        self.assertIn("swapchain_image=", source)
        self.assertIn("source_index=", source)

    def test_provenance_covers_generated_and_source_presents(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn('HostFrameKind::Generated', source)
        self.assertIn('HostFrameKind::Source', source)
        self.assertIn('kind=generated', source)
        self.assertIn('kind=source', source)

    def test_bridge_is_nonblocking_and_does_not_use_files_per_frame(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        start = source.index("publishHostFrameProvenance")
        bridge = source[start:start + 7000]

        self.assertIn("SOCK_DGRAM", bridge)
        self.assertIn("SOCK_NONBLOCK", bridge)
        self.assertIn("sendto", bridge)
        self.assertNotIn("ofstream", bridge)
        self.assertNotIn("fwrite", bridge)
        self.assertNotIn("fsync", bridge)


if __name__ == "__main__":
    unittest.main()
