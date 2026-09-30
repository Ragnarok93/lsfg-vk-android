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

    def test_context_build_contract_is_immutable_and_complete(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        contract = source.index("struct FramegenContextBuildConfig")
        finalized = source.index("const FramegenContextBuildConfig buildConfig{")
        first_ahb = source.index("this->frame_0 = Mini::Image", finalized)
        self.assertLess(contract, finalized)
        self.assertLess(finalized, first_ahb)

        build_block = source[finalized:first_ahb]
        for field in (
            ".performance =",
            ".generationCapacity =",
            ".extent =",
            ".format =",
            ".ahbTransportMode =",
            ".adaptiveFlowEnabled =",
            ".adaptiveFlowPreset =",
            ".adaptiveFlowStates =",
            ".initialFlowScale =",
            ".hdr =",
            ".supportDecision =",
            ".vulkanPath =",
            ".synchronizationPath =",
            ".spirvTargetVersion =",
        ):
            self.assertIn(field, build_block)

        create_end = source.index("const double contextBuildMs", first_ahb)
        construction = source[first_ahb:create_end]
        self.assertIn("buildConfig.generationCapacity", construction)
        self.assertIn("buildConfig.extent", construction)
        self.assertIn("buildConfig.format", construction)
        self.assertIn("buildConfig.ahbTransportMode", construction)
        self.assertIn("buildConfig.adaptiveFlowEnabled", construction)
        self.assertIn("buildConfig.adaptiveFlowStates", construction)
        self.assertIn("buildConfig.performance", construction)

    def test_context_build_telemetry_identifies_exact_final_contract(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        for marker in (
            "immutable_config=1",
            "model=",
            "hdr=",
            "adaptive_flow=",
            "adaptive_flow_preset=",
            "flow_state_values=",
            "support=",
            "fp16=",
            "null_descriptor=",
            "external_sync_fd=",
            "external_opaque_fd=",
            "build_signature_hash=",
            "rebuild_interval_ms=",
            "swapchain_specific_rebuild=1",
        ):
            self.assertIn(marker, source)

    def test_context_creation_reason_distinguishes_create_from_recreate(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("enum class FramegenContextCreationReason", header)
        self.assertIn("FramegenContextCreationReason creationReason", header)
        self.assertIn("FramegenContextCreationReason::SwapchainRecreate", hooks)
        self.assertIn("FramegenContextCreationReason::SwapchainCreate", hooks)
        self.assertIn("framegenContextCreationReasonName(creationReason)", source)


if __name__ == "__main__":
    unittest.main()
