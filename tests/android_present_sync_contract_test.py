#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidPresentSyncContractTest(unittest.TestCase):
    def test_android_generated_and_source_presents_use_distinct_binary_signals(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        android_start = source.index("#ifdef __ANDROID__", source.index("VkResult LsContext::present"))
        desktop_start = source.index("#else", android_start)
        android = source[android_start:desktop_start]

        self.assertIn("pass.prevPostCopySemaphores.at(i) = Mini::Semaphore(info.device);", android)
        self.assertIn("pass.prevPostCopySemaphores.at(i).handle()", android)
        self.assertIn("if (i != 0) waitSemaphores.emplace_back(pass.prevPostCopySemaphores.at(i - 1).handle());", android)
        self.assertIn("VkSemaphore lastPrevPostCopySemaphore =", android)
        self.assertNotIn("VkSemaphore lastPostCopySem =", android)


if __name__ == "__main__":
    unittest.main()
