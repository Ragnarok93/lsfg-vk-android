#!/usr/bin/env python3
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class AndroidCompatibilityPathRoutingTest(unittest.TestCase):
    def test_policy_declares_explicit_platform_paths(self) -> None:
        policy = (ROOT / "include/android_sync_policy.hpp").read_text(
            encoding="utf-8"
        )

        self.assertIn("enum class FramegenCompatibilityPath", policy)
        self.assertIn("AdrenoLatestKnownGood", policy)
        self.assertIn("XclipseCurrent", policy)
        self.assertIn("Generic", policy)
        self.assertIn("selectFramegenCompatibilityPath", policy)
        self.assertIn('"adreno-latest-known-good"', policy)
        self.assertIn('"xclipse-current"', policy)

    def test_context_routes_once_and_logs_the_selected_contract(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("FramegenCompatibilityPath compatibilityPath_", header)
        self.assertIn("selectFramegenCompatibilityPath", source)
        self.assertIn("LSFG compatibility path:", source)
        for field in (
            "gpu=",
            "vendor=",
            "path=",
            "completion=",
            "handoff=",
            "presentation=",
            "retirement=",
            "queue_topology=",
            "behavior_changed=",
        ):
            self.assertIn(field, source)

    def test_xclipse_is_explicitly_behavior_unchanged(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("FramegenCompatibilityPath::XclipseCurrent", source)
        self.assertRegex(
            source,
            r"behavior_changed=.*AdrenoLatestKnownGood",
        )

    def test_batch_sequence_id_crosses_private_and_delivery_paths(self) -> None:
        backend = (ROOT / "framegen/public/lsfg_backend.hpp").read_text(
            encoding="utf-8"
        )
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        quality = (ROOT / "framegen/v3.1_src/context.cpp").read_text(
            encoding="utf-8"
        )
        performance = (ROOT / "framegen/v3.1p_src/context.cpp").read_text(
            encoding="utf-8"
        )

        self.assertIn("uint64_t batchId", backend)
        self.assertIn("deferredAdrenoBatchId_", header)
        self.assertIn("exportedSync.batchId = adaptiveFlowBatch.batchId", quality)
        self.assertIn(
            "exportedSync.batchId = adaptiveFlowBatch.batchId", performance
        )
        self.assertIn("batch_id=", source)
        self.assertIn("stage=adreno-batch-delivery", source)

    def test_present_contract_diagnostics_are_complete(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")

        for field in (
            "swapchain_generation=",
            "requested_present_mode=",
            "wrapper_override_present_mode=",
            "chosen_present_mode=",
            "actual_create_info_present_mode=",
            "image_count=",
            "source_queue=",
            "generated_queue=",
        ):
            self.assertIn(field, hooks)


if __name__ == "__main__":
    unittest.main()
