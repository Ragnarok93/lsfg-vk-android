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
            "Android swapchain/context churn must not unload the private Vulkan runtime",
            "#ifndef __ANDROID__",
            "resetRuntime();",
            "explicit finalize() path",
            "contexts.erase(it)",
        ):
            self.assertIn(marker, text)

        build = (ROOT / "scripts/build/android.sh").read_text(encoding="utf-8")
        self.assertIn("adreno_android_runtime_residency.py", build)
        runtime_patch = build.index("adreno_android_runtime_residency.py")
        runtime_bundle = build.index("apply-adreno-evidence-profile.py")
        self.assertLess(runtime_patch, runtime_bundle, "runtime residency must precede retained runtime composition")

        for backend in ("v3.1_src", "v3.1p_src"):
            source = (ROOT / "framegen" / backend / "lsfg.cpp").read_text(encoding="utf-8")
            self.assertIn("if (contexts.empty())\n        resetRuntime();", source)

    def test_android_config_reload_has_no_constructor_stall_or_build_patch(self) -> None:
        """Config consistency is source-level now; Android must not reintroduce the legacy reload path."""
        transform = ROOT / "scripts/adreno_android_config_reload.py"
        self.assertFalse(
            transform.exists(),
            "obsolete Android config reload source transform must be removed",
        )

        build = (ROOT / "scripts/build/android.sh").read_text(encoding="utf-8")
        self.assertNotIn("adreno_android_config_reload.py", build)

        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        constructor_start = source.index("LsContext::LsContext")
        constructor_end = source.index("LsContext::~LsContext()", constructor_start)
        constructor = source[constructor_start:constructor_end]
        self.assertNotIn(
            "std::this_thread::sleep_for(std::chrono::milliseconds(100))",
            constructor,
        )
        self.assertNotIn("Config::updateConfig", constructor)
        self.assertNotIn("Config::setActive", constructor)
        self.assertNotIn("LSFG_3_1::finalize", constructor)
        self.assertNotIn("LSFG_3_1P::finalize", constructor)



if __name__ == "__main__":
    unittest.main()
