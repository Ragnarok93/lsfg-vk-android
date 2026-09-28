#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidDeferredZeroHistoryContractTest(unittest.TestCase):
    def test_deferred_zero_scripts_remain_available_as_archival_reference(self) -> None:
        for relative in (
            "scripts/adreno_deferred_zero_history.py",
            "scripts/adreno_deferred_zero_history_finalize.py",
            "scripts/adreno_deferred_zero_safe_reprime.py",
            "scripts/adreno_deferred_zero_exit_persistence.py",
        ):
            self.assertTrue((ROOT / relative).exists(), relative)

    def test_restored_build_does_not_compose_deferred_zero_stack(self) -> None:
        build = (ROOT / "scripts/build/android.sh").read_text(encoding="utf-8")
        profile = (ROOT / "scripts/apply-adreno-evidence-profile.py").read_text(
            encoding="utf-8"
        )
        production = build + "\n" + profile
        for forbidden in (
            "apply_deferred_zero_history(root)",
            "apply_deferred_zero_history_finalize(root)",
            "apply_deferred_zero_safe_reprime(root)",
            "apply_deferred_zero_exit_persistence(root)",
        ):
            self.assertNotIn(forbidden, production)

    def test_active_runtime_uses_history_only_not_deferred_zero(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn("AndroidFrameCycleMode::HistoryOnly", source)
        self.assertIn("stage=history-only", source)
        for forbidden in (
            "HistoryMaintenanceState::DeferredZero",
            "rawSourceHistory_",
            "deferred-zero source-only",
            "deferred-zero reprime",
        ):
            self.assertNotIn(forbidden, source)


if __name__ == "__main__":
    unittest.main()
