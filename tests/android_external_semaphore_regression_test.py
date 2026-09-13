#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidExternalSemaphoreRegressionTest(unittest.TestCase):
    def test_input_semaphore_does_not_require_output_semaphore_fds(self) -> None:
        """GPU input handoff is valid even when Android uses fence-based output completion."""
        for relative in (
            "framegen/v3.1_src/context.cpp",
            "framegen/v3.1p_src/context.cpp",
        ):
            source = (ROOT / relative).read_text(encoding="utf-8")
            self.assertNotIn(
                "if (inSem >= 0) outSemaphore = Core::Semaphore(vk.device, outSem.empty() ? -1 : outSem.at(pass));",
                source,
                f"{relative}: an input semaphore must not force import of a synthetic -1 output fd",
            )
            self.assertIn(
                "pass < outSem.size()",
                source,
                f"{relative}: output semaphore import must be gated by an actual output fd",
            )
            self.assertIn(
                "outSem.at(pass) >= 0",
                source,
                f"{relative}: negative output fd sentinels must never reach Core::Semaphore(fd)",
            )

    def test_android_sync_fd_can_drive_cross_device_input_handoff(self) -> None:
        """SYNC_FD must be a first-class Android input handoff when OPAQUE_FD is unavailable."""
        hooks_header = (ROOT / "include/hooks.hpp").read_text(encoding="utf-8")
        hooks_source = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        backend_header = (ROOT / "framegen/public/lsfg_backend.hpp").read_text(encoding="utf-8")
        device_source = (ROOT / "framegen/src/core/device.cpp").read_text(encoding="utf-8")
        mini_header = (ROOT / "include/mini/semaphore.hpp").read_text(encoding="utf-8")
        mini_source = (ROOT / "src/mini/semaphore.cpp").read_text(encoding="utf-8")
        core_header = (ROOT / "framegen/include/core/semaphore.hpp").read_text(encoding="utf-8")
        core_source = (ROOT / "framegen/src/core/semaphore.cpp").read_text(encoding="utf-8")
        outer_source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("androidSyncFdSemaphoreSupported", hooks_header)
        self.assertIn("VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT", hooks_source)
        self.assertIn("externalSemaphoreSyncFd", backend_header)
        self.assertIn("VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT", device_source)

        self.assertIn("VkExternalSemaphoreHandleTypeFlagBits handleType", mini_header)
        self.assertIn("exportFd", mini_header)
        self.assertIn("VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT", mini_source)

        self.assertIn("VkExternalSemaphoreHandleTypeFlagBits handleType", core_header)
        self.assertIn("VK_SEMAPHORE_IMPORT_TEMPORARY_BIT", core_source)
        self.assertIn("VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT", core_source)

        self.assertIn("VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT", outer_source)
        submit_pos = outer_source.index("submitAhbHandoff(info.device")
        export_pos = outer_source.index("exportFd", submit_pos)
        dispatch_pos = outer_source.index("presentContextWithCount", export_pos)
        self.assertLess(submit_pos, export_pos,
            "SYNC_FD export must happen only after the signal operation is pending")
        self.assertLess(export_pos, dispatch_pos,
            "framegen must receive the exported SYNC_FD after the game submission")

    def test_evidence_profiler_does_not_own_sync_fd_behavior(self) -> None:
        """Evidence transforms may profile SYNC_FD but production sources must own selection semantics."""
        source = (ROOT / "scripts/adreno_evidence_capabilities.py").read_text(encoding="utf-8")
        self.assertNotIn("Diagnostic-only: capability is logged but not yet selected", source)
        self.assertNotIn("This build does not enable a SYNC_FD handoff", source)


if __name__ == "__main__":
    unittest.main()
