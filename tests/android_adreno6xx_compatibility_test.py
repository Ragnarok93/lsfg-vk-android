import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class AndroidAdreno6xxCompatibilityContractTest(unittest.TestCase):
    def setUp(self) -> None:
        self.header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        self.source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

    def test_adreno6xx_is_detected_from_physical_device_identity(self) -> None:
        self.assertIn("adreno6xxCompatibilityMode_", self.header)
        self.assertIn("Layer::ovkGetPhysicalDeviceProperties", self.source)
        self.assertIn("0x5143", self.source)
        self.assertIn('Adreno (TM) 6', self.source)

    def test_adreno6xx_does_not_use_async_completion_chain(self) -> None:
        self.assertIn(
            "this->asyncFramegenCompletionEnabled_ = false;",
            self.source,
        )
        self.assertIn("adreno6xx-sync-compat", self.source)

    def test_adreno6xx_history_cycles_use_host_handoff(self) -> None:
        self.assertIn("adrenoHistoryCompatibilityCycle", self.source)
        self.assertIn(
            "this->asyncAhbHandoffEnabled_ && !adrenoHistoryCompatibilityCycle",
            self.source,
        )

    def test_adreno6xx_history_cycles_wait_for_framegen_release(self) -> None:
        history_start = self.source.index("if (historyOnly) {")
        history_end = self.source.index(
            "this->lastDispatchedGeneratedFrameCount_ = generatedFrameCount;",
            history_start,
        )
        history = self.source[history_start:history_end]
        self.assertIn(
            "bool historyRequiresHostCompletionWait = "
            "this->adreno6xxCompatibilityMode_;",
            history,
        )
        self.assertIn("waitContext", history)

    def test_xclipse_path_remains_async_capability_driven(self) -> None:
        # The compatibility gate is strictly Adreno-6xx-specific. Other Android
        # devices still derive handoff/completion policy from their existing
        # external-semaphore capabilities.
        self.assertIn(
            "this->asyncAhbHandoffEnabled_ =",
            self.source,
        )
        self.assertIn(
            "this->asyncFramegenCompletionEnabled_ =",
            self.source,
        )
        self.assertIn("adreno6xxName", self.source)
        self.assertNotIn("xclipseCompatibilityMode_", self.source)


if __name__ == "__main__":
    unittest.main()
