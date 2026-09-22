from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AndroidAdrenoDeferredSingleQueueTest(unittest.TestCase):
    def test_single_queue_adreno_defers_generated_delivery_to_next_source_boundary(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        for token in (
            "deferredAdrenoCompletionEnabled_",
            "deferredAdrenoBatchValid_",
            "deferredAdrenoOutputReadyFds_",
            "deferredAdrenoBatchCompleteFd_",
            "deferredAdrenoPassIndex_",
            "deferredAdrenoGeneratedCount_",
            "deferredAdrenoSourceAge_",
        ):
            self.assertIn(token, header)

        self.assertIn(
            "this->deferredAdrenoCompletionEnabled_ =",
            source,
        )
        self.assertIn(
            "this->conservativeCrossDeviceSync_",
            source,
        )
        self.assertIn(
            "this->syntheticQueue_ == VK_NULL_HANDLE",
            source,
        )
        self.assertIn(
            "presentContextWithCountExportSyncFd",
            source,
        )
        self.assertIn(
            "runtime stage=adreno-deferred-batch-queued",
            source,
        )
        queued_log = source.index("runtime stage=adreno-deferred-batch-queued")
        self.assertIn(
            "if (firstPresentDiagnostic)",
            source[max(0, queued_log - 300):queued_log],
            "deferred Adreno hot path must not synchronously log every batch",
        )

    def test_deferred_batch_never_places_unsignaled_wait_on_primary_queue(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        start = source.index("if (this->deferredAdrenoBatchValid_)")
        end = source.index("const bool conservativePreCopySourceBypass", start)
        deferred = source[start:end]

        self.assertIn("poll(&batchPoll, 1, 0)", deferred)
        self.assertIn("poll(&outputPoll, 1, 0)", deferred)
        self.assertIn("deferredAdrenoOutputEligible_ = false", deferred)
        self.assertIn("conservativeBatchStillInFlight = !batchReady", deferred)
        self.assertNotIn("waitContext(*this->lsfgCtxId, runtimeWaitTimeoutNs()", deferred)

    def test_late_batch_drops_synthetics_but_keeps_source_immediate(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        start = source.index("if (this->deferredAdrenoBatchValid_)")
        end = source.index("const bool conservativePreCopySourceBypass", start)
        deferred = source[start:end]

        self.assertIn("windowGeneratedLateDrops", deferred)
        self.assertIn("windowGeneratedDeadlineDrops", deferred)
        self.assertIn("totalGeneratedDeadlineDrops", deferred)
        self.assertIn("deadlineAdmissionPredictor_.observeDeliveryMiss", deferred)
        self.assertIn("deferredAdrenoSourceAge_", deferred)
        self.assertIn("deferredAdrenoOutputEligible_ = false", deferred)

        bypass_start = source.index("if (conservativePreCopySourceBypass)")
        bypass_end = source.index("// Android path: AHardwareBuffer exchange", bypass_start)
        bypass = source[bypass_start:bypass_end]
        self.assertIn("Layer::ovkQueuePresentKHR(queue, &bypassPresentInfo)", bypass)
        self.assertNotIn("waitContext(", bypass)

    def test_deferred_wsi_rejection_trains_adreno_presentation_capacity(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        start = source.index("if (this->deferredAdrenoBatchValid_)")
        end = source.index("const bool conservativePreCopySourceBypass", start)
        deferred = source[start:end]

        self.assertIn("deferredPresentationAttempted", deferred)
        self.assertIn("deferredWsiDrops", deferred)
        self.assertIn("generatedPresentationCapacityTracker_.observe(", deferred)
        self.assertIn("if (conf.adaptiveFramegen && deferredPresentationAttempted)", deferred)

    def test_deferred_adreno_delivers_ready_temporal_prefix_without_waiting_for_batch_complete(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        start = source.index("if (this->deferredAdrenoBatchValid_)")
        end = source.index("const bool conservativePreCopySourceBypass", start)
        deferred = source[start:end]

        self.assertIn("size_t deferredReadyOutputPrefix = 0", deferred)
        self.assertIn("deferredReadyOutputPrefix < this->deferredAdrenoGeneratedCount_", deferred)
        self.assertIn("const size_t deferredUnreadyOutputCount", deferred)
        self.assertIn(
            "if (deferredReadyOutputPrefix > 0 && deferredPresentationAttempted)",
            deferred,
        )
        self.assertIn("i < deferredReadyOutputPrefix", deferred)
        self.assertIn("conservativeBatchStillInFlight = !batchReady", deferred)
        self.assertIn("deferredAdrenoOutputEligible_ = false", deferred)
        self.assertNotIn(
            "this->deferredAdrenoOutputEligible_ && outputsReady",
            deferred,
            "Adreno deferred delivery must not remain all-or-nothing",
        )

    def test_ready_deferred_batch_delivers_before_current_source_and_retires_pass(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("bool deferredAdrenoOwned{false};", header)

        start = source.index("if (this->deferredAdrenoBatchValid_)")
        end = source.index("const bool conservativePreCopySourceBypass", start)
        deferred = source[start:end]

        self.assertIn("deferredAdrenoPassIndex_", deferred)
        self.assertIn("releasePresentWaitRetirements(imageIdx)", deferred)
        self.assertIn("copyExternalAhbToSwapchain", deferred)
        self.assertIn("Layer::ovkQueuePresentKHR(queue, &deferredPresentInfo)", deferred)
        self.assertIn("submitPassCompletionFence(deferredPass, info.queue.second)", deferred)
        self.assertIn("deferredPass.deferredAdrenoOwned = false", deferred)

    def test_source_ahb_reuse_waits_ready_batch_dependency_without_host_wait(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertIn("deferredAdrenoBatchCompleteSemaphore_", source)
        dependency_start = source.index("std::vector<VkSemaphore> gameRenderSemaphores2")
        dependency_end = source.index("const auto handoffStart", dependency_start)
        dependency = source[dependency_start:dependency_end]

        self.assertIn("deferredAdrenoBatchCompleteReady_", dependency)
        self.assertIn("deferredAdrenoBatchCompleteSemaphore_", dependency)
        self.assertIn("consumeDeferredAdrenoBatchComplete", dependency)

    def test_deferred_batch_blocks_pass_recycle_until_retired(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        recycle_start = source.index("bool LsContext::tryRecyclePass")
        recycle_end = source.index("bool LsContext::submitPassCompletionFence", recycle_start)
        recycle = source[recycle_start:recycle_end]
        self.assertIn("pass.deferredAdrenoOwned", recycle)

    def test_deferred_outputs_are_invalidated_across_resume_or_config_boundary(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        start = source.index("const bool deferredAdrenoBoundaryDiscontinuity")
        end = source.index("if (this->deferredAdrenoBatchValid_)", start)
        guard = source[start:end]
        self.assertIn("runtimeConfigSignature_", guard)
        self.assertIn("runtimeDiagnosticConfigSignature(conf)", guard)
        self.assertIn("deferredAdrenoOutputEligible_ = false", guard)
        self.assertIn("deferredAdrenoOutputReadyFds_", guard)

    def test_adaptive_epoch_reset_drops_stale_outputs_but_preserves_batch_release(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        start = source.index("void LsContext::resetAdaptiveSourceEpoch(bool resetScheduler)")
        end = source.index("void LsContext::enterSourceOnlyBypass()", start)
        reset = source[start:end]

        self.assertIn("deferredAdrenoBatchValid_", reset)
        self.assertIn("deferredAdrenoOutputEligible_ = false", reset)
        self.assertIn("deferredAdrenoOutputReadyFds_", reset)
        self.assertNotIn(
            "deferredAdrenoBatchValid_ = false",
            reset,
            "epoch reset must preserve the private-device batch release dependency",
        )

    def test_source_only_reset_drops_stale_outputs_but_preserves_batch_release(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        start = source.index("void LsContext::enterSourceOnlyBypass()")
        bypass = source[start:]
        self.assertIn("deferredAdrenoOutputEligible_ = false", bypass)
        self.assertIn("deferredAdrenoOutputReadyFds_", bypass)
        self.assertNotIn(
            "deferredAdrenoBatchValid_ = false",
            bypass[:bypass.index("#endif")],
            "source-only transition must preserve the in-flight batch release",
        )

    def test_xclipse_immediate_async_path_is_left_intact(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        selection_start = source.index("this->asyncFramegenCompletionEnabled_ =")
        selection_end = source.index("// The known-good Qualcomm/Adreno path", selection_start)
        selection = source[selection_start:selection_end]
        self.assertIn("!this->conservativeCrossDeviceSync_", selection)

        generated = source[source.index("// 2. Tell framegen"):]
        self.assertIn("if (this->asyncFramegenCompletionEnabled_)", generated)
        self.assertIn("framegenSync.gpuDependenciesExported", generated)


if __name__ == "__main__":
    unittest.main()
