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
            "\n            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT",
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
        # The exact telemetry label is covered by android_sync_policy_test.cpp;
        # this contract verifies the executable routing invariants above rather
        # than coupling the topology to an obsolete policy string.
        self.assertIn("crossDeviceSyncPolicyName", policy)

        log_start = source.index('std::cerr << "lsfg-vk: LSFG compatibility path:"')
        log_end = source.index("// Match the device-proven baseline", log_start)
        compatibility_log = source[log_start:log_end]
        self.assertIn('" presentation=" << compatibilityPresentation', compatibility_log)
        self.assertIn('"generated-before-source-same-call"', source)
        self.assertIn('" synthetic_queue="', compatibility_log)
        self.assertIn('" deadline_semantics="', compatibility_log)
        self.assertIn('"source-protection"', compatibility_log)

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
            "if (sourceProtectionBatchAdmission)",
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

        governor_header_start = header.index("class FixedSourceCadenceGovernor")
        governor_header_end = header.index("\n};", governor_header_start) + len("\n};")
        governor_header = header[governor_header_start:governor_header_end]
        governor_impl_start = scheduler.index("FixedSourceCadenceGovernor::plan")
        governor_impl_end = scheduler.index(
            "FixedSourceCadenceGovernor::reset", governor_impl_start
        )
        governor_impl = scheduler[governor_impl_start:governor_impl_end]
        planning_start = source.index(
            "if (conf.adaptiveFramegen)\n        this->fixedSourceCadenceGovernor_.reset();"
        )
        planning_end = source.index("const auto& adaptiveTelemetry", planning_start)
        fixed_planning = source[planning_start:planning_end]

        self.assertNotIn("sleep_for", governor_header)
        self.assertNotIn("sleep_for", governor_impl)
        self.assertNotIn("sleep_for", fixed_planning)

    def test_zero_generation_keeps_adreno_temporal_history_coherent(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        selection_start = source.index("this->asyncAhbHandoffEnabled_ =")
        selection_end = source.index("const bool xclipseCompatibilityPath", selection_start)
        selection = source[selection_start:selection_end]
        self.assertIn("this->asyncHistoryCompletionEnabled_ = false;", selection)

        handoff_start = source.index("bool useAsyncHandoff =")
        handoff_end = source.index("bool asyncSubmissionIssued", handoff_start)
        handoff = source[handoff_start:handoff_end]
        self.assertIn("!conservativeHistoryGap", handoff)

        history_start = source.index("if (historyOnly)")
        generation_start = source.index(
            "// 2. Tell framegen to generate intermediary frames.", history_start
        )
        history = source[history_start:generation_start]

        self.assertIn("presentContextWithCount(", history)
        self.assertIn("historyRequiresHostCompletionWait", history)
        self.assertIn("noOutSems, 0", history)
        self.assertIn("this->lastDispatchedGeneratedFrameCount_ = 0;", history)
        self.assertIn("++this->conservativeFramegenSourceIndex_;", history)
        self.assertIn("adaptiveSourceResult, \"pre-copy-history-only\"", history)
        self.assertIn(
            "const bool exportHistoryRelease = exportExistingAsyncHistory;",
            history,
        )
        self.assertNotIn("exportProtectedHistoryRelease", history)
        self.assertIn("metrics.windowHistoryHostCompletions++", history)
        self.assertNotIn(
            "return presentCompatibilitySourceOnly("
            "\n            \"compat-adaptive-history-copy\"",
            source,
        )
        self.assertIn("conservativeSourceOnlyWarmup", source)
        self.assertNotIn("deferConservativeWarmupUntilGenerationDemand", source)


    def test_adreno_opaque_source_handoff_matches_september18_order(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        handoff_start = source.index("bool useAsyncHandoff =")
        handoff_end = source.index("bool queuedCopyWithoutHostWait", handoff_start)
        handoff = source[handoff_start:handoff_end]

        # 364178af exported the reusable OPAQUE_FD before queue submission,
        # then submitted the source copy with the real reusable handoff fence.
        # SYNC_FD's post-submit export remains a non-Adreno/Xclipse-only path.
        conservative_branch = handoff.index(
            "if (this->conservativeCrossDeviceSync_) {"
        )
        adreno_export = handoff.index(
            "Mini::Semaphore(info.device, &framegenInputSemaphoreFd)",
            conservative_branch,
        )
        generic_else = handoff.index("} else {", conservative_branch)
        self.assertLess(conservative_branch, adreno_export)
        self.assertLess(adreno_export, generic_else)
        submit = handoff.index("submitAhbHandoff(")
        self.assertLess(adreno_export, submit)
        self.assertIn(
            "this->conservativeCrossDeviceSync_\n"
            "                ? *this->ahbHandoffFence\n"
            "                : VK_NULL_HANDLE",
            handoff,
        )
        self.assertIn(
            "this->conservativeCrossDeviceSync_\n"
            "                ? this->resetHandoffFences\n"
            "                : nullptr",
            handoff,
        )
        post_submit = handoff[submit:]
        conservative_metrics = post_submit.index(
            "if (this->conservativeCrossDeviceSync_) {"
        )
        generic_else = post_submit.index("} else {", conservative_metrics)
        post_submit_export = post_submit.index(
            "framegenInputSemaphore.exportFd", generic_else
        )
        self.assertLess(generic_else, post_submit_export)

    def test_adreno_generated_wsi_matches_september18_presentation_contract(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        start = source.index("// 4. Generated presentation")
        end = source.index("#else", start)
        generated = source[start:end]

        self.assertIn(
            "this->conservativeCrossDeviceSync_\n"
            "                ? runtimeWaitTimeoutNs()\n"
            "                : 0",
            generated,
            "Adreno generated-image acquisition must keep the bounded September 18 wait; "
            "Xclipse/generic keeps the newer nonblocking policy.",
        )
        self.assertIn(
            "if (!this->conservativeCrossDeviceSync_\n"
            "                && (res == VK_NOT_READY || res == VK_TIMEOUT))",
            generated,
        )
        self.assertIn(
            "const void* generatedDownstreamPNext =\n"
            "            this->conservativeCrossDeviceSync_ && i == 0\n"
            "                ? pNext\n"
            "                : nullptr;",
            generated,
        )
        self.assertIn(
            "const void* finalSourceDownstreamPNext =\n"
            "        this->conservativeCrossDeviceSync_\n"
            "            ? (queuedGeneratedFrameCount == 0 ? pNext : nullptr)\n"
            "            : pNext;",
            generated,
        )
        self.assertIn(
            "res = Layer::ovkQueuePresentKHR(generatedPresentQueue, &presentInfo);",
            generated,
        )


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
