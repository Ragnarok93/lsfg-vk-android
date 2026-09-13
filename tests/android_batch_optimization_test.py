#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidBatchOptimizationTest(unittest.TestCase):
    def test_android_last_context_keeps_private_runtime_resident(self) -> None:
        """Android swapchain churn must not destroy the private Vulkan runtime from deleteContext()."""
        transform = ROOT / "scripts/adreno_android_runtime_residency.py"
        self.assertTrue(transform.exists(), "missing Android private-runtime residency transform")
        text = transform.read_text(encoding="utf-8")
        for marker in (
            "framegen runtime retained after last Android context",
            "#ifndef __ANDROID__",
            "resetRuntime();",
            "explicit finalize() path",
            "contexts.erase(it)",
        ):
            self.assertIn(marker, text)

        build = (ROOT / "scripts/build/android.sh").read_text(encoding="utf-8")
        self.assertIn("adreno_android_runtime_residency.py", build)
        profile_gate = build.index('if [[ "${LSFGVK_ZERO_STAGE_PROFILE:-0}" == "1" ]]')
        runtime_patch = build.index("adreno_android_runtime_residency.py")
        self.assertLess(runtime_patch, profile_gate, "runtime residency must apply to every Android build")

        for backend in ("v3.1_src", "v3.1p_src"):
            source = (ROOT / "framegen" / backend / "lsfg.cpp").read_text(encoding="utf-8")
            self.assertIn("if (contexts.empty())\n        resetRuntime();", source)

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
