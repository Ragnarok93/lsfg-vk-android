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

    def test_provenance_carries_context_epoch_across_swapchain_recreation(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn("contextEpoch", source)
        self.assertIn("context_epoch=", source)
        self.assertIn("framegenContextCreateEpoch_", header)
        self.assertIn(".contextEpoch = this->framegenContextCreateEpoch_", source)

    def test_provenance_v3_carries_exact_temporal_intent(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("kHostFrameProvenanceVersion = 3", source)
        self.assertIn("desiredPresentTimeNs", source)
        self.assertIn("desired_present_time_ns=", source)
        self.assertIn("syntheticDesiredTimeNs", source)
        self.assertIn("currentSourceTimeline_.sourceDesiredTimeNs", source)
        self.assertIn("HostFrameKind::Generated, imageIdx", source)
        self.assertIn("HostFrameKind::Source, presentIdx", source)

    def test_fixed_multiplier_delivery_density_is_not_pressure_backed_off(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("fixedGeneratedFrameCount", source)
        self.assertIn("conf.multiplier - 1", source)
        self.assertIn("Fixed multiplier generation is authoritative", source)
        self.assertNotIn("frameQueue", source[source.index("fixedGeneratedFrameCount") - 500:source.index("fixedGeneratedFrameCount") + 1000])

    def test_bridge_is_nonblocking_and_does_not_use_files_per_frame(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        start = source.index("publishHostFrameProvenance")
        bridge = source[start:start + 7000]

        self.assertIn("SOCK_DGRAM", bridge)
        self.assertIn("SOCK_NONBLOCK", bridge)
        self.assertIn("sendto", bridge)
        self.assertIn('getenv("LSFG_PROVENANCE_SOCKET_PATH")', bridge)
        self.assertIn("sun_path", bridge)
        self.assertIn("provenance-socket-send-ok", bridge)
        self.assertIn("provenance-socket-send-failed", bridge)
        self.assertNotIn("ofstream", bridge)
        self.assertNotIn("fwrite", bridge)
        self.assertNotIn("fsync", bridge)


    def test_host_display_feedback_is_nonblocking_and_revision_filtered(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("LSFG_DISPLAY_FEEDBACK_SOCKET", source)
        self.assertIn("pollHostDisplayFeedback", source)
        self.assertIn("packet.configurationRevision != conf.configurationRevision", source)
        self.assertIn("packet.contextEpoch != contextEpoch", source)
        self.assertIn("hostDisplayFeedbackStats.sourceConfirmedDelta", source)
        self.assertIn("hostDisplayFeedbackStats.generatedConfirmedDelta", source)
        self.assertIn("SOCK_NONBLOCK", source)
        self.assertIn("MSG_DONTWAIT", source)
        self.assertIn("host_feedback_generated_confirmed_total=", source)
        self.assertIn("host_feedback_generated_unknown_total=", source)
        self.assertIn("host_feedback_source_confirmed_total=", source)
        self.assertIn("LSFG_HOST_FEEDBACK", source)



if __name__ == "__main__":
    unittest.main()
