#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidFixedModeReadinessTimeoutTest(unittest.TestCase):
    def test_context_reports_only_actual_generated_output(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn("size_t lastGeneratedFrameCount() const", header)
        self.assertIn("size_t lastGeneratedFrameCount_{0};", header)
        self.assertIn("this->lastGeneratedFrameCount_ = 0;", source)
        self.assertIn("this->lastGeneratedFrameCount_ = static_cast<size_t>(conf.multiplier - 1);", source)

    def test_pre_copy_always_produces_independent_source_and_continuation_signals(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        android_start = source.index("#ifdef __ANDROID__", source.index("VkResult LsContext::present"))
        desktop_start = source.index("#else", android_start)
        present = source[android_start:desktop_start]
        self.assertIn("pass.preCopySemaphores.at(0) = Mini::Semaphore(info.device);", present)
        self.assertIn("pass.preCopySemaphores.at(1) = Mini::Semaphore(info.device);", present)
        self.assertIn("pass.preCopySemaphores.at(0).handle(),\n        pass.preCopySemaphores.at(1).handle()", present)
        self.assertNotIn("if (warmupSourceHistory)\n        pass.preCopySemaphores.at(0)", present)

    def test_framegen_timeout_presents_source_and_requests_recreation(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        wait = source.index("if (!framegenReady)")
        generated = source.index("// 4. Copy generated frames", wait)
        timeout = source[wait:generated]
        self.assertIn("framegen-completion-timeout", timeout)
        self.assertIn("pass.preCopySemaphores.at(0).handle()", timeout)
        self.assertIn("Layer::ovkQueuePresentKHR(queue, &timeoutPresentInfo)", timeout)
        self.assertIn("this->lastGeneratedFrameCount_ = 0;", timeout)
        self.assertIn("this->frameIdx++;", timeout)
        self.assertIn("return VK_ERROR_OUT_OF_DATE_KHR;", timeout)
        self.assertNotIn("throw LSFG::vulkan_error(VK_TIMEOUT", timeout)

    def test_stats_contract_matches_gamenative_runtime_gate(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        self.assertIn('<< "active=" << (active ? 1 : 0)', hooks)
        self.assertIn('<< "generation_ready=" << (generationReady ? 1 : 0)', hooks)
        self.assertIn('<< "active=1\\n"', hooks)
        self.assertIn('<< "generation_ready=1\\n"', hooks)
        self.assertIn("swapchain.lastGeneratedFrameCount()", hooks)
        self.assertIn("publishRuntimeState(conf.config_file, false, false", hooks)

    def test_first_generated_cycle_publishes_ready_immediately(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        record_start = hooks.index("void recordSuccessfulOutputCycle")
        record_end = hooks.index("void recordOutputFailure", record_start)
        record = hooks[record_start:record_end]
        self.assertIn("uint64_t generated", record)
        self.assertIn("if (!stats.generationReadyPublished && generated > 0)", record)
        self.assertIn("publishRuntimeState(configFile, true, true", record)
        self.assertIn("stats.generationReadyPublished = true;", record)


if __name__ == "__main__":
    unittest.main()
