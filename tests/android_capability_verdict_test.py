#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class CapabilityVerdictTest(unittest.TestCase):
    def test_public_verdict_has_actionable_capability_fields(self) -> None:
        header = (ROOT / "framegen/public/lsfg_backend.hpp").read_text(encoding="utf-8")
        for marker in (
            "struct FramegenSupportDecision",
            "bool supported",
            "std::string vulkanPath",
            "uint32_t spirvTargetVersion",
            "std::string synchronizationPath",
            "AhbTransportMode ahbMode",
            "bool fp16",
            "bool nullDescriptor",
            "bool externalSyncFd",
            "bool externalOpaqueFd",
            "std::string rejectionReason",
        ):
            self.assertIn(marker, header)

    def test_native_stats_publish_verdict_and_rejection_reason(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        for key in (
            "framegen_support_known=",
            "framegen_supported=",
            "framegen_vulkan_path=",
            "framegen_spirv_target=0x",
            "framegen_sync_path=",
            "framegen_ahb_mode=",
            "framegen_rejection_reason=",
        ):
            self.assertIn(key, hooks)
        self.assertIn(".rejectionReason = e.what()", hooks)

    def test_context_retains_backend_verdict(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn("framegenSupportDecision() const", header)
        self.assertIn("framegenSupportDecision_", header)
        self.assertIn(
            "this->framegenSupportDecision_ = backendDiagnostics.supportDecision;",
            source,
        )


    def test_shader_and_pipeline_failures_keep_module_name(self) -> None:
        source = (ROOT / "framegen/src/pool/shaderpool.cpp").read_text(encoding="utf-8")
        self.assertIn("Shader module creation failed for ", source)
        self.assertIn("Compute pipeline creation failed for ", source)
        self.assertIn("+ name +", source)



if __name__ == "__main__":
    unittest.main()
