#!/usr/bin/env python3
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class AndroidAdrenoS20ReferenceContractTest(unittest.TestCase):
    def test_adreno_ordinary_handoff_prefers_sync_fd_but_completion_is_host_bounded(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        policy = (ROOT / "include/android_sync_policy.hpp").read_text(encoding="utf-8")

        selection_start = source.index("this->asyncAhbHandoffEnabled_ =")
        selection_end = source.index("const bool xclipseCompatibilityPath", selection_start)
        selection = source[selection_start:selection_end]

        # The clean September 18 Android artifact was build-composed with the
        # SYNC_FD handoff transform. On S20+/Turnip it reported OPAQUE_FD
        # unavailable, SYNC_FD available, then used asynchronous handoffs on
        # ordinary generated cycles. Keep OPAQUE_FD only as the capability
        # fallback when SYNC_FD is genuinely unavailable.
        self.assertIn("syncFdHandoffSupported", selection)
        self.assertIn("opaqueFdHandoffSupported", selection)
        self.assertIn(
            "(syncFdHandoffSupported || opaqueFdHandoffSupported)",
            selection,
        )
        self.assertIn(
            "syncFdHandoffSupported\n"
            "            ? VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT",
            selection,
        )

        # Input/source handoff is asynchronous, but the protected Adreno output
        # side remains the bounded private-device host-completion topology.
        self.assertIn(
            "this->asyncFramegenCompletionEnabled_ =\n"
            "        !this->conservativeCrossDeviceSync_",
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

    def test_adreno_adaptive_admission_uses_source_boundary_while_fixed_bypasses_it(self) -> None:
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
        self.assertIn("if (conf.adaptiveFramegen", admission)
        self.assertNotIn(
            "conf.adaptiveFramegen || sourceProtectionBatchAdmission",
            admission,
        )
        self.assertIn("fixedAdrenoHistoricalGeneration", source)
        self.assertIn(
            "fixedAdrenoHistoricalGeneration\n"
            "            ? requestedFixedGeneratedFrameCount",
            source,
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


    def test_adreno_sync_fd_source_handoff_matches_september18_built_runtime(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        begin = source.index("// BEGIN ADRENO_364178AF_EXECUTION")
        end = source.index("// END ADRENO_364178AF_EXECUTION", begin)
        adreno = source[begin:end]

        # September 18's *built* runtime applied adreno_syncfd_handoff.py:
        # create the selected exportable semaphore, submit the source copy, then
        # export SYNC_FD so it represents that exact GPU completion point.
        create = adreno.index(
            "Mini::Semaphore(\n"
            "                    info.device, this->asyncAhbHandoffHandleType_)"
        )
        submit = adreno.index("submitAhbHandoff(", create)
        export_fd = adreno.index("framegenInputSemaphore.exportFd(", submit)
        dispatch = adreno.index("presentContextWithCount(", export_fd)

        self.assertLess(create, submit)
        self.assertLess(submit, export_fd)
        self.assertLess(export_fd, dispatch)
        self.assertIn("*this->ahbHandoffFence", adreno[submit:submit + 900])
        self.assertIn("this->resetHandoffFences", adreno[submit:submit + 900])
        self.assertIn("this->asyncAhbHandoffHandleType_", adreno)
        self.assertIn("VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT", source)
        self.assertNotIn(
            "Mini::Semaphore(info.device, &framegenInputSemaphoreFd)",
            adreno,
        )

        # Generated AHB consumption is still bounded by waitContext on Adreno;
        # this test changes source handoff only, not Xclipse/generic completion.
        self.assertIn("waitContext(*this->lsfgCtxId, framegenCompletionTimeoutNs)", adreno)
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
            "const bool shouldRecyclePass = !this->conservativeCrossDeviceSync_;",
            route_prefix,
        )
        self.assertIn(
            "if (shouldRecyclePass && !this->tryRecyclePass(pass))",
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
        self.assertIn('"adaptive-admission-only"', constructor_log)
        self.assertIn('" fixed_generation="', constructor_log)
        self.assertIn('"historical-direct"', constructor_log)


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


    def test_private_framegen_instance_suppresses_recursive_lsfg_force_enable(self) -> None:
        source = (ROOT / "framegen/src/core/instance.cpp").read_text(encoding="utf-8")

        # 364178af did more than set DISABLE_LSFG: GameNative also force-enables
        # the layer through VK_INSTANCE_LAYERS, so the private compute instance
        # must temporarily remove LSFG from that list and restore it afterwards.
        for token in (
            "privateInstanceEnvironmentMutex",
            'readEnvironment("VK_INSTANCE_LAYERS")',
            'setenv("DISABLE_LSFG", "1", 1)',
            "stripLayerName(",
            '"VK_LAYER_LS_frame_generation"',
            'unsetenv("VK_INSTANCE_LAYERS")',
            'restoreEnvironment("VK_INSTANCE_LAYERS", previousInstanceLayers)',
            'restoreEnvironment("DISABLE_LSFG", previousDisable)',
            "ScopedPrivateInstanceLayerSuppression suppressRecursiveLsfgLayer",
        ):
            self.assertIn(token, source)

        suppression = source.index(
            "ScopedPrivateInstanceLayerSuppression suppressRecursiveLsfgLayer"
        )
        create = source.index("vkCreateInstance(&createInfo", suppression)
        self.assertLess(suppression, create)



if __name__ == "__main__":
    unittest.main()
