#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidWsiTransitionCoalescingContractTest(unittest.TestCase):
    def test_back_to_back_wsi_changes_are_coalesced_after_recreate(self) -> None:
        source = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")

        self.assertIn("wsiRecreateSettleInterval", source)
        self.assertIn("wsiRecreateCooldownUntil", source)
        self.assertIn("wsi-recreate-coalescing", source)
        self.assertIn("RuntimeIoState::Clock::now() < state.wsiRecreateCooldownUntil", source)
        self.assertIn(
            "state.wsiRecreateCooldownUntil = RuntimeIoState::Clock::now() + wsiRecreateSettleInterval",
            source,
        )

        apply_start = source.index("PendingConfigAction applyPendingRuntimeConfig")
        apply_end = source.index("bool applyPendingConfigForSwapchainCreation", apply_start)
        apply_body = source[apply_start:apply_end]
        self.assertIn("if (state.pendingRequiresRecreate", apply_body)
        self.assertIn("return PendingConfigAction::None", apply_body)
        self.assertIn("return PendingConfigAction::Recreate", apply_body)


if __name__ == "__main__":
    unittest.main()
