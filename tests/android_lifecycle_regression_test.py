#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidLifecycleRegressionTest(unittest.TestCase):
    def test_disabled_target_stays_resident_for_hot_enable(self) -> None:
        header = (ROOT / "include/config/config.hpp").read_text(encoding="utf-8")
        config = (ROOT / "src/config/config.cpp").read_text(encoding="utf-8")
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")

        self.assertIn("bool targeted{false}", header)
        self.assertGreaterEqual(config.count(".targeted = true"), 2)
        self.assertIn("!conf.targeted && !conf.enable", main)
        self.assertNotIn(
            'if (!conf.enable && name.second != "benchmark")',
            main,
        )

    def test_hot_reenable_keeps_application_present_mode(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")

        self.assertIn("const bool recreatingExistingSwapchain", hooks)
        self.assertIn("oldConfiguredPresentMode = oldSwapchainState->configuredPresent", hooks)
        self.assertIn("lsfg::wsi::modeRequestForCreate(", hooks)
        self.assertIn("oldConfiguredPresentMode,", hooks)
        self.assertIn("createInfo.presentMode = choosePresentMode(", hooks)
        self.assertIn("stage=swapchain-blit-check-begin", hooks)
        self.assertIn("stage=swapchain-blit-check-ready", hooks)
        self.assertIn("stage=swapchain-downstream-create-begin", hooks)
        self.assertIn("stage=swapchain-downstream-create-return", hooks)

    def test_runtime_state_immediately_reports_generation_readiness(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")

        self.assertIn('"active="', hooks)
        self.assertIn('"generation_ready="', hooks)
        self.assertIn('"state="', hooks)
        self.assertIn('"resident="', hooks)
        self.assertIn('"source_only="', hooks)
        self.assertIn('"generation_initialized="', hooks)
        self.assertIn('"generated_presented="', hooks)
        self.assertIn('"degraded="', hooks)
        self.assertIn("publishRuntimeState", hooks)
        self.assertIn('publishRuntimeState(configFile, "degraded"', hooks)
        self.assertIn("const bool generationActive = activeConf.multiplier > 1", hooks)
        self.assertIn('generationActive ? "generating" : "source_only"', hooks)

    def test_resident_context_sizes_present_resources_to_runtime_capacity(self) -> None:
        """A 2x resident context must already own the slots needed by later 3x/4x hot toggles."""
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        allocation_start = source.index("for (size_t i = 0; i < 8; i++)")
        allocation_end = source.index("\n    }\n}", allocation_start)
        allocation = source[allocation_start:allocation_end]

        self.assertGreaterEqual(allocation.count("resize(runtimeMultiplier - 1)"), 5)
        self.assertNotIn("resize(conf.multiplier - 1)", allocation)


    def test_generation_off_uses_resident_native_present_bypass(self) -> None:
        """LSFG-off must bypass framegen without rebuilding or retuning the active WSI."""
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")

        self.assertNotIn("const auto createSourceOnly", hooks)
        self.assertNotIn('return createSourceOnly("generation-off")', hooks)

        bypass_start = hooks.index("if (conf.targeted && conf.multiplier <= 1)")
        bypass_end = hooks.index("        try {", bypass_start)
        bypass = hooks[bypass_start:bypass_end]
        self.assertIn("Layer::ovkQueuePresentKHR(queue, pPresentInfo)", bypass)
        self.assertIn("recordSuccessfulOutputCycle(*state, *state->context", bypass)
        self.assertNotIn("state->context->present(", bypass)

        # Logical FIFO is physically MAILBOX-backed for targeted Android LSFG,
        # so Off keeps the same nonblocking resident WSI instead of retuning it.
        self.assertIn("residentFifoMailboxBacked", hooks)
        self.assertIn("activeConf.targeted", hooks)
        self.assertIn("configuredPresentMode == VK_PRESENT_MODE_FIFO_KHR", hooks)
        self.assertIn("VK_PRESENT_MODE_MAILBOX_KHR", hooks)


if __name__ == "__main__":
    unittest.main()
