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

        present_start = source.index("VkResult LsContext::present")
        handoff = source.index("submitAndWaitForAhbHandoff", present_start)
        adaptive_zero = source.index("if (adaptiveZeroGeneration)", handoff)
        zero_end = source.index("if (warmupSourceHistory)", adaptive_zero)
        zero_block = source[adaptive_zero:zero_end]

        self.assertGreater(adaptive_zero, handoff)
        self.assertIn("presentContextWithCount", zero_block)
        self.assertIn("stage=adaptive-history-advance", zero_block)
        self.assertIn("requiresSourceHistoryWarmup_ = false", zero_block)
        self.assertNotIn("requiresSourceHistoryWarmup_ = true", zero_block)
        self.assertNotIn("enterSourceOnlyBypass", zero_block)

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
            present_start = source.index("void Context::present")
            wait_start = source.index("bool Context::waitForLastPresent", present_start)
            present = source[present_start:wait_start]

            self.assertNotIn(stale_early_return, present, source_path.as_posix())

            mipmaps = present.index("this->mipmaps.Dispatch")
            alpha = present.index("this->alpha.at", mipmaps)
            beta = present.index("this->beta.Dispatch", alpha)
            zero_finish = present.index("if (generationCount == 0)", beta)
            second_stage = present.index(
                "for (size_t pass = 0; pass < generationCount; pass++)", zero_finish)
            zero_block = present[zero_finish:second_stage]

            self.assertLess(mipmaps, alpha)
            self.assertLess(alpha, beta)
            self.assertIn("preprocessingFence.wait", zero_block)
            self.assertIn("framegenWaitTimeoutNs()", zero_block)
            self.assertIn("this->frameIdx++", zero_block)
            self.assertIn("return", zero_block)

            android_release = present[beta:zero_finish]
            self.assertIn("generationCount == 0 && !this->transportOnly", android_release)
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
            present_start = source.index("void Context::present")
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

    def test_generated_passes_reuse_completion_fences(self) -> None:
        backend_sources = (
            ROOT / "framegen/v3.1_src/context.cpp",
            ROOT / "framegen/v3.1p_src/context.cpp",
        )

        for source_path in backend_sources:
            source = source_path.read_text(encoding="utf-8")
            present_start = source.index("void Context::present")
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

    def test_source_only_bypass_remains_lifecycle_reset(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        bypass = source[source.index("void LsContext::enterSourceOnlyBypass"):]
        self.assertIn("requiresSourceHistoryWarmup_ = true", bypass)
        self.assertIn("previousSourceCopySignalValid_ = false", bypass)


if __name__ == "__main__":
    unittest.main()
