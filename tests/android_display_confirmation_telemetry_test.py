#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidDisplayConfirmationTelemetryTest(unittest.TestCase):
    def test_xclipse_uses_google_display_timing_as_observability_only(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        # Display confirmation is independent from the dormant adaptive pacing
        # experiment. It must remain telemetry-only and must not alter the
        # restored WSI cadence contract.
        self.assertIn("generatedDisplayConfirmationEnabled_", header)
        self.assertIn("getPastPresentationTimingGoogle_", header)
        self.assertIn("generatedDisplayPendingIds_", header)
        self.assertIn("!this->conservativeCrossDeviceSync_", source)
        self.assertIn('backend=google-display-timing', source)
        self.assertIn(".desiredPresentTime = 0", source)

    def test_generated_presents_receive_unique_ids_and_are_polled_asynchronously(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("generatedDisplayConfirmationPNext", source)
        self.assertIn("trackGeneratedDisplayPresent", source)
        self.assertIn("pollGeneratedDisplayConfirmations", source)
        self.assertIn("vkGetPastPresentationTimingGOOGLE", source)
        self.assertIn("VkPastPresentationTimingGOOGLE", source)
        self.assertNotIn("vkQueueWaitIdle", source[source.index("pollGeneratedDisplayConfirmations"):source.index("pollGeneratedDisplayConfirmations") + 4500])
        self.assertNotIn("vkWaitForFences", source[source.index("pollGeneratedDisplayConfirmations"):source.index("pollGeneratedDisplayConfirmations") + 4500])

    def test_actual_present_time_is_the_display_confirmation_boundary(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("windowGeneratedDisplayNotShown", header)
        self.assertIn("totalGeneratedDisplayNotShown", header)
        self.assertIn("timing.actualPresentTime != 0", source)
        self.assertIn("generated_display_not_shown=", source)
        self.assertIn("generated_display_pending=", source)
        self.assertIn("display-timing-confirmed", source)
        self.assertIn("display-timing-pending", source)

    def test_adreno_proven_path_is_not_instrumented_with_present_timing(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        enable = source.index("generatedDisplayConfirmationEnabled_")
        block = source[enable:enable + 1400]
        self.assertIn("!this->conservativeCrossDeviceSync_", block)


if __name__ == "__main__":
    unittest.main()
