#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidAdaptiveHistoryContractTest(unittest.TestCase):
    def test_zero_generation_advances_history_without_source_pacing(self) -> None:
        header = (ROOT / "include/adaptive_scheduler.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        self.assertNotIn("delayUntilNextSourceOutput", header)
        self.assertNotIn("delayUntilNextSourceOutput", source)
        self.assertNotIn("std::this_thread::sleep_for(delay)", source)

        context_header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        self.assertIn(
            "kSourceHistoryWarmupFrames = 4",
            context_header,
            "Startup/discontinuity warmup must replace the initially duplicated or stale motion slot before generation",
        )

        present_start = source.index("VkResult LsContext::present")
        handoff = source.index("submitAndWaitForAhbHandoff", present_start)
        self.assertIn("enum class AndroidFrameCycleMode", source)
        self.assertIn("AndroidFrameCycleMode::HistoryOnly", source)
        history_only = source.index("if (historyOnly)", handoff)
        history_end = source.index(
            "// 2. Tell framegen to generate intermediary frames.", history_only
        )
        history_block = source[history_only:history_end]

        self.assertGreater(history_only, handoff)
        self.assertIn("presentContextWithCountExportSyncFd", history_block)
        self.assertIn("framegenBatchCompleteSemaphore", history_block)
        self.assertIn("historyRequiresHostCompletionWait", history_block)
        self.assertIn("stage=history-only", history_block)
        self.assertIn("sourceHistoryWarmupRemaining_ > 0", history_block)
        self.assertIn("--this->sourceHistoryWarmupRemaining_", history_block)
        self.assertIn("history_warmup_remaining=", history_block)
        timeout_start = history_block.index("if (!historyReady)")
        timeout_recovery = history_block[timeout_start:]
        self.assertIn("kSourceHistoryWarmupFrames", timeout_recovery)
        self.assertIn("requiresSourceHistoryWarmup_ = true", timeout_recovery)
        self.assertIn("history-completion-timeout", timeout_recovery)
        self.assertNotIn("enterSourceOnlyBypass", history_block)
        self.assertNotIn("compat-adaptive-history-copy", source)
        self.assertIn("presentContextWithCount(", history_block)

    def test_protected_adreno_scene_reprime_consumes_both_guard_slots(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        # A hard protected-Adreno scene transition must be recovered through
        # the real zero-generation framegen history path. Ordinary lifecycle
        # warmup is a source-only compatibility path and cannot seed both
        # private LSFG source slots or deliver resetTemporalHistory.
        arm_token = "this->adaptiveSceneTransitionGuard_.arm(true);"
        arm_offsets = []
        search_from = 0
        while True:
            offset = source.find(arm_token, search_from)
            if offset < 0:
                break
            arm_offsets.append(offset)
            search_from = offset + len(arm_token)
        self.assertEqual(len(arm_offsets), 2)
        for arm_offset in arm_offsets:
            arm_end = source.index("this->lsfgOutputCadenceTracker_.configure(", arm_offset)
            arm_block = source[arm_offset:arm_end]
            self.assertIn("this->sourceHistoryWarmupRemaining_ = 0;", arm_block)
            self.assertIn("this->requiresSourceHistoryWarmup_ = false;", arm_block)
            self.assertNotIn("sourceHistoryWarmupRemaining_ = std::max", arm_block)

        island_start = source.index("// BEGIN ADRENO_364178AF_EXECUTION")
        island_end = source.index("// END ADRENO_364178AF_EXECUTION", island_start)
        island = source[island_start:island_end]
        zero_start = island.index(
            "if (generatedFrameCount == 0 && !sourceHistoryWarmupActive)"
        )
        zero_end = island.index(
            "// September 18 performs one real-source copy/present warmup",
            zero_start,
        )
        zero_history = island[zero_start:zero_end]

        self.assertIn("presentContextWithCount(", zero_history)
        self.assertIn(
            "adaptiveSceneTransitionGuard_.consumeSourceOnly()",
            zero_history,
            "Each successful protected zero-history cycle must consume exactly one scene-reprime slot",
        )
        self.assertIn("adaptive-scene-reprime", zero_history)
        self.assertIn("action=consume", zero_history)
        self.assertIn(
            "this->adaptiveSceneTransitionGuard_.sourceOnlyRemaining()",
            zero_history,
        )

        self.assertIn(
            "this->adaptiveSceneTransitionGuard_.sourceOnlyRemaining() == 2",
            source,
            "Only the first scene-reprime history cycle should request the hard temporal reset",
        )

    def test_framegen_zero_generation_refreshes_temporal_preprocessing(self) -> None:
        backend_sources = (
            ROOT / "framegen/v3.1_src/context.cpp",
            ROOT / "framegen/v3.1p_src/context.cpp",
        )
        stale_early_return = (
            "if (generationCount == 0) {\n"
            "        this->frameIdx++;\n"
            "        return;\n"
            "    }"
        )

        for source_path in backend_sources:
            source = source_path.read_text(encoding="utf-8")
            present_start = source.index("Context::present(")
            wait_start = source.index("bool Context::waitForLastPresent", present_start)
            present = source[present_start:wait_start]

            self.assertNotIn(stale_early_return, present, source_path.as_posix())

            mipmaps = present.index("this->mipmaps.Dispatch")
            alpha = present.index("this->alpha.at(6 - i).Dispatch(", mipmaps)
            self.assertIn("if (generationCount > 0)", present, source_path.as_posix())
            beta_guard = present.index("if (generationCount > 0)", alpha)
            beta = present.index("this->beta.Dispatch", beta_guard)
            zero_finish = present.index("if (generationCount == 0)", beta)
            second_stage = present.index(
                "for (size_t pass = 0; pass < generationCount; pass++)", zero_finish)
            zero_block = present[zero_finish:second_stage]

            self.assertLess(mipmaps, alpha)
            self.assertLess(alpha, beta_guard)
            self.assertLess(beta_guard, beta)
            self.assertLess(beta, zero_finish)
            self.assertIn("exportZeroHistorySync", zero_block)
            self.assertIn("data.batchCompleteSemaphore", zero_block)
            self.assertIn("exportedSync.gpuDependenciesExported = true", zero_block)
            self.assertIn("data.shouldWait = true", zero_block)
            self.assertIn("preprocessingFence.wait", zero_block)
            self.assertIn("framegenWaitTimeoutNs()", zero_block)
            self.assertIn("this->frameIdx++", zero_block)
            self.assertIn("return", zero_block)

            android_release = present[beta:zero_finish]
            self.assertIn("generationCount == 0 && !this->inputCopyRequired", android_release)
            self.assertIn("add_external_release", android_release)
            self.assertIn("this->inImg_0", android_release)
            self.assertIn("this->inImg_1", android_release)

    def test_zero_generation_reuses_preprocessing_fences(self) -> None:
        backend_pairs = (
            (
                ROOT / "framegen/v3.1_include/v3_1/context.hpp",
                ROOT / "framegen/v3.1_src/context.cpp",
            ),
            (
                ROOT / "framegen/v3.1p_include/v3_1p/context.hpp",
                ROOT / "framegen/v3.1p_src/context.cpp",
            ),
        )

        for header_path, source_path in backend_pairs:
            header = header_path.read_text(encoding="utf-8")
            source = source_path.read_text(encoding="utf-8")
            present_start = source.index("Context::present(")
            wait_start = source.index("bool Context::waitForLastPresent", present_start)
            present = source[present_start:wait_start]
            zero_start = present.index("if (generationCount == 0)")
            second_stage = present.index(
                "for (size_t pass = 0; pass < generationCount; pass++)", zero_start)
            zero_block = present[zero_start:second_stage]

            self.assertIn("Core::Fence preprocessingFence", header, header_path.as_posix())
            self.assertNotIn(
                "Core::Fence preprocessingFence(vk.device)",
                zero_block,
                source_path.as_posix(),
            )
            self.assertIn(
                "data.preprocessingFence.reset(vk.device)",
                zero_block,
                source_path.as_posix(),
            )
            self.assertIn("data.preprocessingFence", zero_block, source_path.as_posix())
            self.assertIn("data.preprocessingFence.wait", zero_block, source_path.as_posix())
            self.assertIn("exportZeroHistorySync", zero_block, source_path.as_posix())

    def test_generated_passes_reuse_completion_fences(self) -> None:
        backend_sources = (
            ROOT / "framegen/v3.1_src/context.cpp",
            ROOT / "framegen/v3.1p_src/context.cpp",
        )

        for source_path in backend_sources:
            source = source_path.read_text(encoding="utf-8")
            present_start = source.index("Context::present(")
            wait_start = source.index("bool Context::waitForLastPresent", present_start)
            present = source[present_start:wait_start]
            pass_start = present.index("for (size_t pass = 0; pass < generationCount; pass++)")
            pass_block = present[pass_start:]

            self.assertGreaterEqual(
                source.count("for (auto& completionFence : data.completionFences)"),
                2,
                source_path.as_posix(),
            )
            self.assertNotIn(
                "completionFence = Core::Fence(vk.device)",
                pass_block,
                source_path.as_posix(),
            )
            self.assertIn(
                "completionFence.reset(vk.device)",
                pass_block,
                source_path.as_posix(),
            )

    def test_generated_passes_reuse_internal_semaphores(self) -> None:
        backend_sources = (
            ROOT / "framegen/v3.1_src/context.cpp",
            ROOT / "framegen/v3.1p_src/context.cpp",
        )

        for source_path in backend_sources:
            source = source_path.read_text(encoding="utf-8")
            present_start = source.index("Context::present(")
            wait_start = source.index("bool Context::waitForLastPresent", present_start)
            present = source[present_start:wait_start]

            self.assertGreaterEqual(
                source.count("for (auto& internalSemaphore : data.internalSemaphores)"),
                2,
                source_path.as_posix(),
            )
            self.assertNotIn(
                "data.internalSemaphores.at(i) = Core::Semaphore(vk.device)",
                present,
                source_path.as_posix(),
            )
            self.assertIn(
                "data.internalSemaphores.begin()",
                present,
                source_path.as_posix(),
            )
            self.assertIn(
                "{ internalSemaphore }",
                present,
                source_path.as_posix(),
            )

    def test_source_only_bypass_remains_lifecycle_reset(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        bypass = source[source.index("void LsContext::enterSourceOnlyBypass"):]
        self.assertIn(
            "this->conservativeCrossDeviceSync_ ? 1U : kSourceHistoryWarmupFrames",
            bypass,
        )
        self.assertIn("kSourceHistoryWarmupFrames", bypass)
        self.assertIn(
            "requiresSourceHistoryWarmup_ =\n"
            "        this->sourceHistoryWarmupRemaining_ > 0",
            bypass,
        )
        self.assertIn("previousSourceCopySignalValid_ = false", bypass)
        self.assertIn("adaptiveScheduler_.reset()", bypass)
        self.assertIn("deadlineAdmissionPredictor_.reset()", bypass)
        self.assertIn("adaptiveFlowController_.reset()", bypass)
        self.assertIn("runtimeMetrics.hasLastSourcePresent = false", bypass)
        self.assertIn("advanceAdaptiveFlowTimingEpoch()", bypass)


if __name__ == "__main__":
    unittest.main()
