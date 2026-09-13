#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidBatchOptimizationTest(unittest.TestCase):
    def test_android_last_context_keeps_private_runtime_resident(self) -> None:
        """Android swapchain churn must not destroy the private Vulkan runtime from deleteContext()."""
        for backend, namespace in (("v3.1_src", "LSFG_3_1"), ("v3.1p_src", "LSFG_3_1P")):
            source = (ROOT / "framegen" / backend / "lsfg.cpp").read_text(encoding="utf-8")
            delete_start = source.index(f"void {namespace}::deleteContext")
            finalize_start = source.index(f"void {namespace}::finalize", delete_start)
            delete_body = source[delete_start:finalize_start]
            finalize_body = source[finalize_start:]

            self.assertIn("contexts.erase(it)", delete_body)
            self.assertIn("#ifndef __ANDROID__", delete_body)
            self.assertIn("framegen runtime retained after last Android context", delete_body)
            self.assertIn("resetRuntime()", delete_body)
            self.assertIn("resetRuntime()", finalize_body)

            android_guard = delete_body.index("#ifndef __ANDROID__")
            reset = delete_body.index("resetRuntime()", android_guard)
            android_else = delete_body.index("#else", android_guard)
            android_end = delete_body.index("#endif", android_else)
            self.assertLess(reset, android_else)
            self.assertLess(android_else, android_end)

    def test_android_config_reload_skips_legacy_100ms_sleep(self) -> None:
        """GameNative writes config atomically; Android must not add a fixed transition stall."""
        transform = ROOT / "scripts/adreno_android_config_reload.py"
        self.assertTrue(transform.exists(), "missing Android config reload hardening transform")
        text = transform.read_text(encoding="utf-8")
        for marker in (
            "android-config-reload-no-sleep",
            "std::this_thread::sleep_for(std::chrono::milliseconds(100));",
            "#ifndef __ANDROID__",
        ):
            self.assertIn(marker, text)

        build = (ROOT / "scripts/build/android.sh").read_text(encoding="utf-8")
        self.assertIn("adreno_android_config_reload.py", build)
        profile_gate = build.index('if [[ "${LSFGVK_ZERO_STAGE_PROFILE:-0}" == "1" ]]')
        config_patch = build.index("adreno_android_config_reload.py")
        self.assertLess(config_patch, profile_gate, "config hardening must apply to every Android build")

    def test_transport_only_zero_history_signals_shared_ahb_release_early(self) -> None:
        """TransportOnly completion FD should cover AHB copy/release, not private mipmap/alpha work."""
        transform = ROOT / "scripts/adreno_transport_release_overlap.py"
        self.assertTrue(transform.exists(), "missing TransportOnly release-overlap transform")
        text = transform.read_text(encoding="utf-8")

        for marker in (
            "transportOnlyHistoryOverlap",
            "transportReleaseCommandBuffer",
            "transportReleaseFence",
            "transportReadySemaphore",
            "zero-history-sync-fd transport-release-submit",
            "activeSharedHistoryInput",
            "activePrivateHistoryInput",
            "historyCompletionSemaphore",
            "preprocessingFence",
        ):
            self.assertIn(marker, text)

        self.assertIn("data.transportReleaseCommandBuffer.submit", text)
        self.assertIn("data.cmdBuffer1.submit", text)
        self.assertIn("{data.historyCompletionSemaphore,data.transportReadySemaphore}", text)
        self.assertIn("{data.transportReadySemaphore}", text)
        self.assertIn("transportReleaseFence.wait", text)

        bundle = (ROOT / "scripts/apply-adreno-evidence-profile.py").read_text(encoding="utf-8")
        self.assertIn("apply_transport_release_overlap(root)", bundle)
        self.assertLess(
            bundle.index("apply_slot_aware_zero_history(root)"),
            bundle.index("apply_transport_release_overlap(root)"),
            "release overlap must patch the already slot-aware TransportOnly path",
        )


if __name__ == "__main__":
    unittest.main()
