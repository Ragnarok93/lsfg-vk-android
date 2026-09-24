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
        log_end = source.index(
            'std::cerr << "lsfg-vk: Android AHB context created', log_start
        )
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

        begin = source.index("// BEGIN ADRENO_364178AF_EXECUTION")
        end = source.index("// END ADRENO_364178AF_EXECUTION", begin)
        adreno = source[begin:end]

        # 364178af creates/exports the reusable OPAQUE_FD semaphore before the
        # game-device source-copy submit, and that submit carries the real
        # reusable handoff fence. No post-submit SYNC_FD export exists here.
        export = adreno.index(
            "Mini::Semaphore(info.device, &framegenInputSemaphoreFd)"
        )
        submit = adreno.index("submitAhbHandoff(", export)
        self.assertLess(export, submit)
        self.assertIn("*this->ahbHandoffFence", adreno[submit:submit + 700])
        self.assertIn("this->resetHandoffFences", adreno[submit:submit + 700])
        self.assertNotIn("framegenInputSemaphore.exportFd", adreno)
        self.assertNotIn("VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT", adreno)

        # Generic/Xclipse remains outside the island and may keep its newer
        # handle-specific post-submit export behavior.
        generic = source[end:]
        self.assertIn("framegenInputSemaphore.exportFd", generic)

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


    def test_adreno_startup_and_reentry_warmup_match_september_18(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        constructor_start = source.index("this->compatibilityPath_ =")
        constructor_end = source.index(
            'std::cerr << "lsfg-vk: Android AHB context created', constructor_start
        )
        constructor = source[constructor_start:constructor_end]
        self.assertIn(
            "if (this->conservativeCrossDeviceSync_)",
            constructor,
        )
        self.assertIn(
            "this->sourceHistoryWarmupRemaining_ = 0;",
            constructor,
            "364178af starts Adreno generation without a synthetic startup warmup.",
        )
        self.assertIn(
            "this->requiresSourceHistoryWarmup_ = false;",
            constructor,
        )

        bypass_start = source.index("void LsContext::enterSourceOnlyBypass()")
        bypass = source[bypass_start:]
        self.assertIn(
            "this->conservativeCrossDeviceSync_ ? 1U : kSourceHistoryWarmupFrames",
            bypass,
            "September 18 re-entry requires exactly one real-source warmup on Adreno.",
        )

    def test_adreno_execution_island_matches_september_18_transport_contract(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        begin = source.index("// BEGIN ADRENO_364178AF_EXECUTION")
        end = source.index("// END ADRENO_364178AF_EXECUTION", begin)
        adreno = source[begin:end]

        # New governors may choose the admitted/interpolation count and Flow
        # metadata before entering this block. They may not alter the proven
        # September 18 cross-device/presentation topology inside it.
        self.assertIn("generatedFrameCount > 0", adreno)
        self.assertIn("!sourceHistoryWarmupActive", adreno)
        self.assertNotIn("conservativeFramegenSourceIndex_", adreno)
        self.assertNotIn("conservativePendingBatchComplete", adreno)
        self.assertNotIn("conservativePendingHistoryComplete", adreno)
        self.assertNotIn("deferredAdreno", adreno)
        self.assertNotIn("crossFrameWaitRetentions", adreno)

        self.assertIn("submitAhbHandoff(", adreno)
        self.assertIn("submitAndWaitForAhbHandoff(", adreno)
        self.assertIn("presentContextWithCount(", adreno)
        self.assertNotIn("presentContextWithCountExportSyncFd(", adreno)
        self.assertIn("waitContext(*this->lsfgCtxId", adreno)

        self.assertIn("runtimeWaitTimeoutNs()", adreno)
        self.assertNotIn("generatedAcquireTimeoutNs = 0", adreno)
        self.assertNotIn("VK_NOT_READY", adreno)
        self.assertNotIn("generated-wsi-drop", adreno)

        self.assertIn(
            "const void* generatedDownstreamPNext = i == 0 ? pNext : nullptr;",
            adreno,
        )
        self.assertIn(
            "const void* finalSourceDownstreamPNext =\n"
            "            generatedFrameCount == 0 ? pNext : nullptr;",
            adreno,
        )
        self.assertIn("Layer::ovkQueuePresentKHR(queue, &presentInfo)", adreno)
        self.assertIn("Layer::ovkQueuePresentKHR(queue, &finalPresentInfo)", adreno)

    def test_adreno_execution_island_is_reached_before_modern_runtime_plumbing(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        begin = source.index("// BEGIN ADRENO_364178AF_EXECUTION")
        end = source.index("// END ADRENO_364178AF_EXECUTION", begin)
        generic = source.index(
            "// Android path: AHardwareBuffer exchange between two VkDevices.",
            end,
        )
        island = source[begin:end]

        self.assertLess(begin, generic)
        self.assertIn("if (this->conservativeCrossDeviceSync_)", island)

        # Modern pass-slot/retirement plumbing is deliberately not allowed to
        # gate or mutate the September 18 Qualcomm transaction.
        present_start = source.index("VkResult LsContext::present")
        route_prefix = source[present_start:begin]
        self.assertIn(
            "if (!this->conservativeCrossDeviceSync_ && !this->tryRecyclePass(pass))",
            route_prefix,
        )
        self.assertNotIn("tryRecyclePass(pass)", island)
        self.assertNotIn("retainPresentWait(", island)
        self.assertNotIn("armPassGpuRetirement(", island)

        # Runtime logs make the compatibility island and governor boundary
        # explicit in device captures.
        constructor_start = source.index(
            'std::cerr << "lsfg-vk: LSFG compatibility path:"'
        )
        constructor_end = source.index(
            'std::cerr << "lsfg-vk: Android AHB context created',
            constructor_start,
        )
        constructor_log = source[constructor_start:constructor_end]
        self.assertIn('" execution_reference="', constructor_log)
        self.assertIn('"364178af-sep18"', constructor_log)
        self.assertIn('" governor_adapter="', constructor_log)
        self.assertIn('"admission-only"', constructor_log)


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
