#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidDeferredZeroReprimeGuardContractTest(unittest.TestCase):
    def test_reprime_current_source_capture_is_local_async_and_guarded(self) -> None:
        transform = ROOT / "scripts/adreno_deferred_zero_reprime_guard.py"
        self.assertTrue(transform.exists(), "missing guarded re-prime transition transform")
        text = transform.read_text(encoding="utf-8")

        for marker in (
            "deferred-zero reprime capture-record",
            "deferred-zero reprime capture-submit",
            "deferred-zero reprime capture-submitted",
            "deferred-zero reprime replay-enter",
            "pass.preCopyBuf.submit",
            "reprime-fallback stage=",
        ):
            self.assertIn(marker, text)

        replacement_start = text.index("new_transition =")
        replacement_end = text.index("text = once(", replacement_start)
        transition = text[replacement_start:replacement_end]
        self.assertNotIn("submitAndWaitForAhbHandoff", transition)
        self.assertNotIn("waitForAhbHandoff", transition)
        self.assertLess(transition.index("try {"), transition.index("copySwapchainToRawHistory"))
        self.assertLess(transition.index("copySwapchainToRawHistory"), transition.index("replay-enter"))

    def test_guard_transform_runs_after_sync_fix_and_before_finalize(self) -> None:
        bundle = (ROOT / "scripts/apply-adreno-evidence-profile.py").read_text(encoding="utf-8")
        self.assertIn("apply_deferred_zero_reprime_guard(root)", bundle)
        self.assertLess(
            bundle.index("apply_deferred_zero_reprime_sync(root)"),
            bundle.index("apply_deferred_zero_reprime_guard(root)"),
        )
        self.assertLess(
            bundle.index("apply_deferred_zero_reprime_guard(root)"),
            bundle.index("apply_deferred_zero_history_finalize(root)"),
        )


if __name__ == "__main__":
    unittest.main()
