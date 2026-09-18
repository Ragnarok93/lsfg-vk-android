#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CONTEXTS = (
    ROOT / "framegen/v3.1_src/context.cpp",
    ROOT / "framegen/v3.1p_src/context.cpp",
)


class NonblockingZeroTimeoutPollTest(unittest.TestCase):
    def test_zero_timeout_still_polls_completion_fences(self) -> None:
        for path in CONTEXTS:
            source = path.read_text(encoding="utf-8")
            start = source.index("bool Context::waitForLastPresent")
            end = source.index("bool Context::waitForCompletion", start)
            body = source[start:end]
            with self.subTest(path=path.name):
                self.assertIn("if (timeoutNs == 0)", body)
                zero = body.index("if (timeoutNs == 0)")
                poll = body.index(".wait(vk.device, 0)", zero)
                deadline = body.index("const auto deadline", zero)
                self.assertLess(poll, deadline)

    def test_positive_timeout_keeps_bounded_wait(self) -> None:
        for path in CONTEXTS:
            source = path.read_text(encoding="utf-8")
            start = source.index("bool Context::waitForLastPresent")
            end = source.index("bool Context::waitForCompletion", start)
            body = source[start:end]
            with self.subTest(path=path.name):
                self.assertIn("const auto deadline", body)
                self.assertIn("if (now >= deadline)", body)
                self.assertIn("static_cast<uint64_t>(remaining)", body)


if __name__ == "__main__":
    unittest.main()
