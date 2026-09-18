#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidNonblockingGeneratedPipelineTest(unittest.TestCase):
    def test_final_transform_owns_nonblocking_generated_delivery(self) -> None:
        transform_path = ROOT / "scripts/adreno_nonblocking_generated_pipeline.py"
        self.assertTrue(transform_path.exists(), "missing nonblocking generated pipeline transform")
        transform = transform_path.read_text(encoding="utf-8")
        build = (ROOT / "scripts/build/android.sh").read_text(encoding="utf-8")

        self.assertIn("adreno_nonblocking_generated_pipeline.py", build)
        self.assertGreater(
            build.index("adreno_nonblocking_generated_pipeline.py"),
            build.index("apply-adreno-evidence-profile.py"),
        )
        self.assertLess(
            build.index("adreno_nonblocking_generated_pipeline.py"),
            build.index("apply-android-command-buffer-reuse.py"),
        )

        for marker in (
            "nonblocking-generated-pipeline",
            "pendingSourceValid_",
            "framegenInFlight_",
            "framegen-late-drop",
            "waitContext(*this->lsfgCtxId, 0)",
            "generatedCompletionFds",
            "presentContextWithCountAndCompletionFds",
            "sourceWait=buffered-source",
        ):
            self.assertIn(marker, transform)

        # The hot path may poll a fence with timeout=0, but it must never sleep
        # or perform the old bounded completion wait before advancing a real frame.
        self.assertNotIn("std::this_thread::sleep_for", transform)
        self.assertNotIn("framegenCompletionTimeoutNs", transform)

    def test_hitching_pacing_owners_are_not_modified_by_native_transform(self) -> None:
        transform = (ROOT / "scripts/adreno_nonblocking_generated_pipeline.py").read_text(
            encoding="utf-8"
        )
        for forbidden in (
            "PresentExtension.java",
            "ShmFramePacer.java",
            "XServerView.java",
            "setFrameRateLimit(",
        ):
            self.assertNotIn(forbidden, transform)


if __name__ == "__main__":
    unittest.main()
