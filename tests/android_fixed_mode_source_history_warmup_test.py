#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidFixedModeSourceHistoryWarmupTest(unittest.TestCase):
    def test_context_starts_with_source_history_warmup_required(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        self.assertIn("bool requiresSourceHistoryWarmup_{true};", header)

    def test_first_source_frame_populates_history_without_framegen_dispatch(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        android_start = source.index("#ifdef __ANDROID__", source.index("VkResult LsContext::present"))
        desktop_start = source.index("#else", android_start)
        present = source[android_start:desktop_start]

        self.assertIn("const bool warmupSourceHistory = this->requiresSourceHistoryWarmup_;", present)
        self.assertIn("pass.preCopySemaphores.at(0) = Mini::Semaphore(info.device);", present)
        self.assertIn("preCopySignals", present)
        self.assertIn("if (warmupSourceHistory)", present)
        warmup_start = present.index("if (warmupSourceHistory)", present.index("source-ahb-handoff-ready"))
        dispatch_start = present.index("framegen-dispatch-begin")
        self.assertLess(warmup_start, dispatch_start)

        warmup = present[warmup_start:dispatch_start]
        self.assertIn("this->requiresSourceHistoryWarmup_ = false;", warmup)
        self.assertIn("pass.preCopySemaphores.at(0).handle()", warmup)
        self.assertIn("Layer::ovkQueuePresentKHR(queue, &warmupPresentInfo)", warmup)
        self.assertIn("this->frameIdx++;", warmup)
        self.assertIn("return warmupResult;", warmup)
        self.assertNotIn("presentContext", warmup)

    def test_warmup_preserves_independent_source_and_continuation_signals(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        android_start = source.index("#ifdef __ANDROID__", source.index("VkResult LsContext::present"))
        desktop_start = source.index("#else", android_start)
        present = source[android_start:desktop_start]

        self.assertIn("pass.preCopySemaphores.at(0).handle(),\n        pass.preCopySemaphores.at(1).handle()", present)
        self.assertIn("this->frameIdx > 0", present)
        self.assertIn(".preCopySemaphores.at(1).handle()", present)


if __name__ == "__main__":
    unittest.main()
