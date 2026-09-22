#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidAdrenoCompatibilityRestoreTest(unittest.TestCase):
    def test_backend_exposes_driver_id_for_policy_selection(self) -> None:
        backend = (ROOT / "framegen/public/lsfg_backend.hpp").read_text(encoding="utf-8")
        device = (ROOT / "framegen/src/core/device.cpp").read_text(encoding="utf-8")
        self.assertIn("VkDriverId driverId", backend)
        self.assertIn("driverProperties.driverID", device)

    def test_context_selects_cross_device_policy_without_global_async_rollback(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn("conservativeCrossDeviceSync_", header)
        self.assertIn("requiresConservativeCrossDeviceSync", source)
        self.assertIn("backendDiagnostics.driverId", source)
        self.assertIn("sync_policy=", source)
        self.assertIn("!this->conservativeCrossDeviceSync_", source)
        self.assertIn("this->asyncAhbHandoffEnabled_", source)
        self.assertIn("this->asyncFramegenCompletionEnabled_", source)

    def test_conservative_warmup_is_source_only_but_fractional_gaps_keep_history(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn("conservativeSourceOnlyWarmup", source)
        self.assertIn("conservativeFractionalHistoryGap", source)
        self.assertIn("conservativeTrueSourceOnlyCycle", source)
        helper = source.split("const auto presentCompatibilitySourceOnly", 1)[1].split(
            "if (conservativeSourceOnlyWarmup)", 1
        )[0]
        warmup = source.split("if (conservativeSourceOnlyWarmup)", 1)[1].split(
            "if (historyOnly)", 1
        )[0]
        self.assertIn("Layer::ovkQueuePresentKHR", helper)
        self.assertIn("presentCompatibilitySourceOnly", warmup)
        self.assertNotIn("presentContextWithCount", warmup)
        self.assertNotIn("presentContextWithCountExportSyncFd", warmup)

    def test_adreno_uses_known_good_single_source_reprime_without_startup_delay(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn("kConservativeSourceReprimeFrames = 1", source)
        self.assertIn(
            "this->conservativeCrossDeviceSync_ ? kConservativeSourceReprimeFrames",
            source,
        )
        selection = source.split(
            "this->conservativeCrossDeviceSync_ =", 1
        )[1].split("std::cerr << \"lsfg-vk: Android AHB context created", 1)[0]
        self.assertIn("sourceHistoryWarmupRemaining_ = 0", selection)
        self.assertIn("requiresSourceHistoryWarmup_ = false", selection)

    def test_non_async_history_preprocessing_waits_before_ahb_reuse(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        history = source.split("if (historyOnly)", 1)[1].split(
            "// 2. Tell framegen", 1
        )[0]
        fallback = history.split("} else {", 1)[1]
        self.assertIn("historyRequiresHostCompletionWait = true", fallback)
        self.assertIn("waitContext", history)

    def test_generated_compatibility_path_retains_bounded_completion_wait(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        generated = source.split("// 2. Tell framegen", 1)[1]
        self.assertIn(
            "bool requireHostCompletionWait = !this->asyncFramegenCompletionEnabled_",
            generated,
        )
        self.assertIn("runtimeWaitTimeoutNs()", generated)
        self.assertIn("waitContext", generated)

    def test_xclipse_async_path_is_not_removed(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn("syncFdHandoffSupported", source)
        self.assertIn("presentContextWithCountExportSyncFd", source)
        self.assertIn("VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT", source)


if __name__ == "__main__":
    unittest.main()
