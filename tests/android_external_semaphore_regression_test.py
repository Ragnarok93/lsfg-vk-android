#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidExternalSemaphoreRegressionTest(unittest.TestCase):
    def test_input_semaphore_does_not_require_output_semaphore_fds(self) -> None:
        """GPU input handoff stays independent from generated-output completion FDs."""
        for relative in (
            "framegen/v3.1_src/context.cpp",
            "framegen/v3.1p_src/context.cpp",
        ):
            source = (ROOT / relative).read_text(encoding="utf-8")
            self.assertNotIn(
                "if (inSem >= 0) outSemaphore = Core::Semaphore(vk.device, outSem.empty() ? -1 : outSem.at(pass));",
                source,
                f"{relative}: input handoff must not force a synthetic -1 output fd import",
            )
            self.assertIn("pass < outSem.size()", source, relative)
            self.assertIn("outSem.at(pass) >= 0", source, relative)

    def test_external_fd_import_preserves_handle_specific_transfer_semantics(self) -> None:
        """OPAQUE_FD stays permanent; SYNC_FD uses the required temporary import."""
        source = (ROOT / "framegen/src/core/semaphore.cpp").read_text(encoding="utf-8")

        for marker in (
            "VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT",
            "VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT",
            "vkImportSemaphoreFdKHR",
            "::close(fd);",
            ".handleType = handleType",
            "VK_SEMAPHORE_IMPORT_TEMPORARY_BIT",
        ):
            self.assertIn(marker, source)

        self.assertIn(
            "syncFd ? VK_SEMAPHORE_IMPORT_TEMPORARY_BIT : 0U",
            source,
        )

    def test_sync_fd_input_handoff_exports_after_signal_submission(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        wrapper = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        backend = (ROOT / "framegen/public/lsfg_backend.hpp").read_text(encoding="utf-8")

        self.assertIn("VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT", hooks)
        self.assertIn("androidSyncFdSemaphoreSupported", hooks)
        self.assertIn("externalSemaphoreSyncFd", backend)

        selection = wrapper.index("syncFdHandoffSupported")
        submit = wrapper.index("submitAhbHandoff(", selection)
        export_fd = wrapper.index(".exportFd(", submit)
        dispatch = wrapper.index("presentContextWithCount(", export_fd)
        self.assertLess(submit, export_fd)
        self.assertLess(export_fd, dispatch)
        self.assertIn("handoffTypeName", wrapper)

    def test_completed_sync_fd_minus_one_is_still_imported(self) -> None:
        for relative in (
            "framegen/v3.1_src/context.cpp",
            "framegen/v3.1p_src/context.cpp",
        ):
            source = (ROOT / relative).read_text(encoding="utf-8")
            self.assertIn("const bool hasInputSemaphore", source)
            self.assertIn("inSem == -1", source)
            self.assertIn("VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT", source)
            self.assertIn("if (!hasInputSemaphore) waits.clear();", source)

    def test_generated_output_completion_uses_sync_fd_without_normal_host_wait(self) -> None:
        wrapper = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        backend = (ROOT / "framegen/public/lsfg_backend.hpp").read_text(encoding="utf-8")

        self.assertIn("AndroidFrameSyncFds", backend)
        self.assertIn("presentContextWithCountExportSyncFd", wrapper)
        self.assertIn("gpuDependenciesExported", wrapper)
        self.assertIn("framegenBatchCompleteSemaphore", wrapper)
        self.assertIn("requireHostCompletionWait", wrapper)
        self.assertIn("if (requireHostCompletionWait)", wrapper)

        dispatch = wrapper.index("presentContextWithCountExportSyncFd")
        fallback = wrapper.index("if (requireHostCompletionWait)", dispatch)
        self.assertLess(dispatch, fallback)
        self.assertIn("outputReadyWaitValid", wrapper[dispatch:fallback])

    def test_batch_completion_is_consumed_by_next_source_copy(self) -> None:
        wrapper = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        producer = wrapper.index("pass.framegenBatchCompleteSemaphore = Mini::Semaphore")
        consumer = wrapper.index("previousPass->framegenBatchCompleteSemaphore.handle()")
        clear = wrapper.index("previousPass->framegenBatchCompleteValid = false", consumer)
        self.assertLess(consumer, producer)
        self.assertLess(consumer, clear)

    def test_direct_input_ahbs_are_released_before_batch_complete_signal(self) -> None:
        for relative in (
            "framegen/v3.1_src/context.cpp",
            "framegen/v3.1p_src/context.cpp",
        ):
            source = (ROOT / relative).read_text(encoding="utf-8")
            output_copy = source.index("if (this->outputCopyRequired)")
            final_release = source.index(
                "pass + 1 == generationCount && !this->inputCopyRequired",
                output_copy,
            )
            batch_signal = source.index(
                "signals.emplace_back(data.batchCompleteSemaphore)",
                final_release,
            )
            self.assertLess(final_release, batch_signal)
            self.assertIn("add_external_release(", source[final_release:batch_signal])
            self.assertIn("this->inImg_0", source[final_release:batch_signal])
            self.assertIn("this->inImg_1", source[final_release:batch_signal])

    def test_async_input_export_failure_fails_open_without_reusing_host_fence(self) -> None:
        wrapper = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        async_submit = wrapper.index(
            "submitAhbHandoff(info.device, pass.preCopyBuf, info.queue.second"
        )
        export_fd = wrapper.index("framegenInputSemaphore.exportFd", async_submit)
        fail_open = wrapper.index("pre-copy-syncfd-fail-open", export_fd)

        self.assertIn("VK_NULL_HANDLE, nullptr", wrapper[async_submit:export_fd])
        self.assertNotIn("waitForAhbHandoff(", wrapper[export_fd:fail_open])
        self.assertIn("requiresSourceHistoryWarmup_ = true", wrapper[export_fd:fail_open])

    def test_submit_hot_path_preserves_batch_complete_signal(self) -> None:
        transform = (ROOT / "scripts/apply-android-submit-hot-path.py").read_text(
            encoding="utf-8"
        )
        self.assertIn("data.submitSignalSemaphores.reserve(2)", transform)
        self.assertIn("exportSyncFdOutputs && pass + 1 == generationCount", transform)
        self.assertIn("data.batchCompleteSemaphore", transform)

    def test_zero_generation_exports_batch_completion_without_host_wait(self) -> None:
        wrapper = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn(
            "presentContextWithCountExportSyncFd(\n"
            "                    *this->lsfgCtxId, framegenInputSemaphoreFd, 0",
            wrapper,
        )
        self.assertIn("zero-history completion SYNC_FD import failed", wrapper)

        for relative in (
            "framegen/v3.1_src/context.cpp",
            "framegen/v3.1p_src/context.cpp",
        ):
            source = (ROOT / relative).read_text(encoding="utf-8")
            zero_start = source.index("if (generationCount == 0)")
            zero_end = source.index(
                "const std::vector<Core::Semaphore> activeInternalSemaphores",
                zero_start,
            )
            zero = source[zero_start:zero_end]
            self.assertIn("exportZeroHistorySync", zero, relative)
            self.assertIn("data.batchCompleteSemaphore", zero, relative)
            self.assertIn("exportedSync.batchCompleteFd", zero, relative)
            self.assertIn("exportedSync.gpuDependenciesExported = true", zero, relative)
            submit = zero.index("data.cmdBuffer1.submit")
            export_fd = zero.index("data.batchCompleteSemaphore.exportFd", submit)
            self.assertLess(submit, export_fd, relative)

    def test_restored_build_does_not_compose_experimental_sync_stacks(self) -> None:
        """Experimental zero-history/nonblocking stacks remain archival, not production composition."""
        build = (ROOT / "scripts/build/android.sh").read_text(encoding="utf-8")
        profile = (ROOT / "scripts/apply-adreno-evidence-profile.py").read_text(encoding="utf-8")

        for forbidden in (
            "scripts/adreno_nonblocking_generated_pipeline.py",
            "apply_deferred_zero_history(root)",
            "apply_async_zero_history(root)",
            "apply_slot_aware_zero_history(root)",
            "apply_transport_release_overlap(root)",
            "apply_syncfd_handoff(root)",
        ):
            self.assertNotIn(forbidden, build + "\n" + profile)

    def test_admission_preserves_planned_interpolation_denominator(self) -> None:
        public_headers = (
            ROOT / "framegen/public/lsfg_3_1.hpp",
            ROOT / "framegen/public/lsfg_3_1p.hpp",
        )
        context_sources = (
            ROOT / "framegen/v3.1_src/context.cpp",
            ROOT / "framegen/v3.1p_src/context.cpp",
        )
        for path in public_headers:
            source = path.read_text(encoding="utf-8")
            self.assertIn("size_t interpolationGenerationCount = 0", source, path.as_posix())
        for path in context_sources:
            source = path.read_text(encoding="utf-8")
            self.assertIn("const size_t interpolationCount", source, path.as_posix())
            self.assertIn("pass, interpolationCount", source, path.as_posix())


if __name__ == "__main__":
    unittest.main()
