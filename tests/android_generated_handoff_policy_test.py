#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidGeneratedHandoffPolicyTest(unittest.TestCase):
    def test_turnip_generated_cycles_keep_proven_host_fence_path(self) -> None:
        sync = (ROOT / "scripts/adreno_syncfd_handoff.py").read_text(encoding="utf-8")
        zero = (ROOT / "scripts/adreno_async_zero_history.py").read_text(encoding="utf-8")

        self.assertIn("generatedAsyncAhbHandoffEnabled_", sync)
        self.assertIn('driverName.find("Turnip")', sync)
        self.assertIn("std::string::npos", sync)

        # Generated and zero-history policies are intentionally independent:
        # Turnip generated work uses the proven host-fence path, while zero
        # history may still use GPU SYNC_FD completion.
        self.assertIn(
            "generatedFrameCount>0 && this->generatedAsyncAhbHandoffEnabled_",
            zero,
        )
        self.assertIn(
            "adaptiveZeroGeneration && this->asyncZeroHistoryEnabled_",
            zero,
        )

    def test_non_turnip_backends_retain_capability_gated_async_handoff(self) -> None:
        sync = (ROOT / "scripts/adreno_syncfd_handoff.py").read_text(encoding="utf-8")
        self.assertIn(
            "this->generatedAsyncAhbHandoffEnabled_ =",
            sync,
        )
        self.assertIn(
            "this->asyncAhbHandoffEnabled_",
            sync,
        )
        self.assertIn(
            'backendDiagnostics.driverName.find("Turnip") == std::string::npos',
            sync,
        )


if __name__ == "__main__":
    unittest.main()
