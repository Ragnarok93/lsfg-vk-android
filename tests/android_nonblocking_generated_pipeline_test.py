#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidNonblockingGeneratedPipelineTest(unittest.TestCase):
    def test_experimental_transform_remains_archival_only(self) -> None:
        transform_path = ROOT / "scripts/adreno_nonblocking_generated_pipeline.py"
        self.assertTrue(transform_path.exists(), "missing archival nonblocking transform")
        transform = transform_path.read_text(encoding="utf-8")
        build = (ROOT / "scripts/build/android.sh").read_text(encoding="utf-8")
        profile = (ROOT / "scripts/apply-adreno-evidence-profile.py").read_text(
            encoding="utf-8"
        )

        self.assertIn("nonblocking-generated-pipeline", transform)
        self.assertNotIn(
            "scripts/adreno_nonblocking_generated_pipeline.py",
            build + "\n" + profile,
        )

    def test_restored_runtime_keeps_blocking_completion_until_safety_gate(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn("framegenCompletionTimeoutNs", source)
        self.assertIn("waitContext(*this->lsfgCtxId, framegenCompletionTimeoutNs)", source)
        self.assertNotIn("nonblocking-generated-pipeline active=1", source)
        self.assertNotIn("framegen-late-drop", source)

    def test_archival_transform_never_modifies_java_pacing_owners(self) -> None:
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
