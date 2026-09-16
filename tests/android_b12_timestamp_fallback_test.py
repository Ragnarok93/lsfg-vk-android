#!/usr/bin/env python3
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TRANSFORM = ROOT / "scripts/apply-b12-dual-stage-profile.py"
FALLBACK = ROOT / "scripts/apply-b12-unreported-timestamp-fallback.py"


class B12TimestampFallbackContractTest(unittest.TestCase):
    def test_b12_probes_unreported_timestamps_without_changing_clean_runtime(self) -> None:
        self.assertTrue(FALLBACK.exists(), FALLBACK.as_posix())
        required = (
            Path("framegen/include/core/timestampquerypool.hpp"),
            Path("framegen/src/core/timestampquerypool.cpp"),
            Path("framegen/v3.1_include/v3_1/context.hpp"),
            Path("framegen/v3.1_src/context.cpp"),
            Path("framegen/v3.1_include/v3_1/shaders/mipmaps.hpp"),
            Path("framegen/v3.1_src/shaders/mipmaps.cpp"),
            Path("framegen/v3.1_include/v3_1/shaders/beta.hpp"),
            Path("framegen/v3.1_src/shaders/beta.cpp"),
            Path("framegen/v3.1p_include/v3_1p/context.hpp"),
            Path("framegen/v3.1p_src/context.cpp"),
            Path("framegen/v3.1p_include/v3_1p/shaders/mipmaps.hpp"),
            Path("framegen/v3.1p_src/shaders/mipmaps.cpp"),
            Path("framegen/v3.1p_include/v3_1p/shaders/beta.hpp"),
            Path("framegen/v3.1p_src/shaders/beta.cpp"),
        )
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            for rel in required:
                target = temp_root / rel
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(ROOT / rel, target)

            for _ in range(2):
                subprocess.run(
                    [sys.executable, str(TRANSFORM), "--root", str(temp_root)],
                    check=True,
                )
                subprocess.run(
                    [sys.executable, str(FALLBACK), "--root", str(temp_root)],
                    check=True,
                )

            header = (temp_root / "framegen/include/core/timestampquerypool.hpp").read_text(
                encoding="utf-8"
            )
            source = (temp_root / "framegen/src/core/timestampquerypool.cpp").read_text(
                encoding="utf-8"
            )
            perf_context = (temp_root / "framegen/v3.1p_src/context.cpp").read_text(
                encoding="utf-8"
            )

            self.assertIn("bool allowUnreportedTimestamps = false", header)
            self.assertIn("vkGetPhysicalDeviceQueueFamilyProperties2", source)
            self.assertIn("timestampPeriod", source)
            self.assertIn("vkCreateCommandPool", source)
            self.assertIn("vkCmdWriteTimestamp", source)
            self.assertIn("vkQueueSubmit", source)
            self.assertIn("vkGetQueryPoolResults", source)
            self.assertIn("b12-timestamp-capability", source)
            self.assertIn("b12-timestamp-fallback", source)
            self.assertIn("result=enabled", source)
            self.assertIn("result=disabled", source)
            self.assertIn("TimestampQueryPool(vk.device, 2, true)", perf_context)

            # Evidence-only fallback must not rewrite the clean checked-in core.
            clean_header = (ROOT / "framegen/include/core/timestampquerypool.hpp").read_text(
                encoding="utf-8"
            )
            self.assertNotIn("allowUnreportedTimestamps", clean_header)

    def test_optional_mipmaps_executable_capture_is_b12_only(self) -> None:
        build = (ROOT / "scripts/build/android.sh").read_text(encoding="utf-8")
        self.assertIn("LSFGVK_B12_MIPMAPS_EXEC_PROFILE", build)
        self.assertIn("apply-candidate-b6-pipeline-executable-profile.py", build)
        self.assertIn("B12_MIPMAPS_EXEC_PROFILE", build)
        self.assertIn("requires LSFGVK_B12_DUAL_STAGE_PROFILE=1", build)


if __name__ == "__main__":
    unittest.main()
