#!/usr/bin/env python3
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

class AndroidA6xxTransportContract(unittest.TestCase):
    def test_external_semaphore_fd_is_capability_gated(self):
        hooks = (ROOT / "src/hooks.cpp").read_text()
        device = (ROOT / "framegen/src/core/device.cpp").read_text()
        backend = (ROOT / "framegen/public/lsfg_backend.hpp").read_text()
        self.assertIn("supportsOpaqueFdExternalSemaphore", hooks)
        self.assertIn("VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT", hooks)
        self.assertIn("VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT", hooks)
        self.assertIn("VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME", hooks)
        self.assertIn("externalSemaphoreFd", backend)
        self.assertIn("probeOpaqueFdExternalSemaphore", device)

    def test_fast_path_replaces_both_host_waits_with_gpu_semaphores(self):
        context = (ROOT / "src/context.cpp").read_text()
        self.assertIn("externalSemaphoreFdSync_", context)
        self.assertIn("Mini::Semaphore(info.device, &preCopySemaphoreFd)", context)
        self.assertIn("renderSemaphoreFds", context)
        self.assertIn("if (!useExternalSemaphoreSync)", context)
        self.assertIn("waitContext", context)
        self.assertIn("submitAndWaitForAhbHandoff", context)
        self.assertIn("postCopyWaits.emplace_back(pass.renderSemaphores.at(i).handle())", context)
        self.assertIn("previousAsyncReuseSignalValid_", context)

    def test_transport_only_refreshes_one_history_slot_after_priming(self):
        for rel in ("framegen/v3.1_src/context.cpp", "framegen/v3.1p_src/context.cpp"):
            context = (ROOT / rel).read_text()
            self.assertIn("refreshBothInputs", context)
            self.assertIn("refreshInput0", context)
            self.assertIn("refreshInput1", context)
            self.assertIn("transportInputsPrimed_ = false", context)
            self.assertIn("transportInputsPrimed_ = true", context)

    def test_zero_generation_ticks_keep_history_parity_aligned(self):
        outer = (ROOT / "src/context.cpp").read_text()
        self.assertGreaterEqual(outer.count("advanceFramegenCadence();"), 2)
        for rel in ("framegen/v3.1_src/context.cpp", "framegen/v3.1p_src/context.cpp"):
            context = (ROOT / rel).read_text()
            self.assertIn("if (generationCount == 0)", context)
            self.assertIn("this->frameIdx++", context)

if __name__ == "__main__":
    unittest.main()
