#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidExternalSemaphoreRegressionTest(unittest.TestCase):
    def test_input_semaphore_does_not_require_output_semaphore_fds(self) -> None:
        """GPU input handoff stays independent from generated-output completion FDs."""
        for relative in (
            "framegen/v3.1_src/context.cpp",
            "framegen/v3.1p_src/context.cpp",
        ):
            source = (ROOT / relative).read_text(encoding="utf-8")
            self.assertNotIn(
                "if (inSem >= 0) outSemaphore = Core::Semaphore(vk.device, outSem.empty() ? -1 : outSem.at(pass));",
                source,
                f"{relative}: input handoff must not force a synthetic -1 output fd import",
            )
            self.assertIn("pass < outSem.size()", source, relative)
            self.assertIn("outSem.at(pass) >= 0", source, relative)

    def test_opaque_fd_import_has_explicit_failure_ownership(self) -> None:
        """OPAQUE_FD import must consume the fd on success and close it on pre-import failure."""
        source = (ROOT / "framegen/src/core/semaphore.cpp").read_text(encoding="utf-8")

        for marker in (
            "VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT",
            "vkImportSemaphoreFdKHR",
            "::close(fd);",
            "Successful import transfers ownership of fd to Vulkan.",
        ):
            self.assertIn(marker, source)

        self.assertNotIn(
            "VK_SEMAPHORE_IMPORT_TEMPORARY_BIT",
            source,
            "OPAQUE_FD imports are permanent payload imports, not temporary SYNC_FD imports",
        )

    def test_restored_build_does_not_compose_experimental_sync_stacks(self) -> None:
        """Experimental zero-history/nonblocking stacks remain archival, not production composition."""
        build = (ROOT / "scripts/build/android.sh").read_text(encoding="utf-8")
        profile = (ROOT / "scripts/apply-adreno-evidence-profile.py").read_text(encoding="utf-8")

        for forbidden in (
            "scripts/adreno_nonblocking_generated_pipeline.py",
            "apply_deferred_zero_history(root)",
            "apply_async_zero_history(root)",
            "apply_slot_aware_zero_history(root)",
            "apply_transport_release_overlap(root)",
            "apply_syncfd_handoff(root)",
        ):
            self.assertNotIn(forbidden, build + "\n" + profile)


if __name__ == "__main__":
    unittest.main()
