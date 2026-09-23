#!/usr/bin/env python3
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class AndroidAdrenoS20ReferenceContractTest(unittest.TestCase):
    def test_adreno_ordinary_handoff_is_opaque_fd_but_completion_is_host_bounded(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        policy = (ROOT / "include/android_sync_policy.hpp").read_text(encoding="utf-8")

        selection_start = source.index("this->asyncAhbHandoffEnabled_ =")
        selection_end = source.index("const bool xclipseCompatibilityPath", selection_start)
        selection = source[selection_start:selection_end]

        self.assertNotIn(
            "!this->conservativeCrossDeviceSync_\n"
            "        && gameGetSemaphoreFd != nullptr",
            selection,
        )
        self.assertIn("if (this->conservativeCrossDeviceSync_)", selection)
        self.assertIn("opaqueFdHandoffSupported", selection)
        self.assertIn(
            "this->asyncAhbHandoffHandleType_ ="
            "\n        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT",
            selection,
        )
        self.assertIn(
            "this->asyncFramegenCompletionEnabled_ ="
            "\n        !this->conservativeCrossDeviceSync_",
            selection,
        )
        self.assertIn("this->deferredAdrenoCompletionEnabled_ = false;", selection)

        constructor = source[source.index("this->compatibilityPath_ ="):selection_start]
        self.assertIn("this->syntheticQueue_ = VK_NULL_HANDLE;", constructor)
        self.assertIn(
            "useAdrenoSyntheticQueue",
            source,
            "The dormant queue selector may remain for non-Adreno code, "
            "but the Adreno compatibility path must not select it.",
        )
        self.assertIn("sync_policy=host-fence-input-host-completion-adreno", policy)

    def test_adreno_admission_uses_source_boundary_for_fixed_and_adaptive_work(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        policy_start = source.index("const bool sourceProtectionBatchAdmission")
        policy_end = source.index("double computeReadyBudgetMs", policy_start)
        policy = source[policy_start:policy_end]
        self.assertIn(
            "const bool sourceProtectionBatchAdmission ="
            "\n        this->conservativeCrossDeviceSync_;",
            policy,
        )
        self.assertNotIn("deferredAdrenoCompletionEnabled_", policy)

        admission_start = source.index("// Active deadline admission")
        admission_end = source.index(
            "const auto& outputCadenceForPresentation", admission_start
        )
        admission = source[admission_start:admission_end]
        self.assertIn(
            "conf.adaptiveFramegen || sourceProtectionBatchAdmission",
            admission,
        )
        self.assertIn("sourceBudgetMs", admission)
        self.assertIn(
            "sourceProtectionBatchAdmission"
            " && plannedBatchDecision.valid",
            admission,
        )
        self.assertIn(
            "deadlineSemantics ="
            "\n        sourceProtectionBatchAdmission",
            source,
        )

    def test_fixed_mode_restores_cadence_governor_without_source_pacing(self) -> None:
        header = (ROOT / "include/adaptive_scheduler.hpp").read_text(
            encoding="utf-8"
        )
        context_header = (ROOT / "include/context.hpp").read_text(
            encoding="utf-8"
        )
        scheduler = (ROOT / "src/adaptive_scheduler.cpp").read_text(
            encoding="utf-8"
        )
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("class FixedSourceCadenceGovernor", header)
        self.assertIn("fixedSourceCadenceGovernor_", context_header)
        self.assertIn("FixedSourceCadenceGovernor::plan", scheduler)
        self.assertIn("fixedSourceCadenceGovernor_.plan(", source)
        self.assertIn("fixed_generation_limit=", source)
        self.assertNotIn("sleep_for", header + scheduler + source)

    def test_zero_generation_keeps_adreno_temporal_history_coherent(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        history_start = source.index("if (historyOnly)")
        generation_start = source.index(
            "// 2. Tell framegen to generate intermediary frames.", history_start
        )
        history = source[history_start:generation_start]

        self.assertIn("presentContextWithCount(", history)
        self.assertIn("historyRequiresHostCompletionWait = true", history)
        self.assertIn("generatedFrameCount = 0", history)
        self.assertNotIn(
            "return presentCompatibilitySourceOnly("
            "\n            \"compat-adaptive-history-copy\"",
            source,
        )
        self.assertIn("conservativeSourceOnlyWarmup", source)

    def test_xclipse_async_selection_remains_capability_driven(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        selection_start = source.index("this->asyncFramegenCompletionEnabled_ =")
        selection_end = source.index(
            "const bool xclipseCompatibilityPath", selection_start
        )
        selection = source[selection_start:selection_end]
        self.assertIn("syncFdHandoffSupported", selection)
        self.assertIn("gameImportSemaphoreFd != nullptr", selection)
        self.assertIn("!this->conservativeCrossDeviceSync_", selection)


if __name__ == "__main__":
    unittest.main()
