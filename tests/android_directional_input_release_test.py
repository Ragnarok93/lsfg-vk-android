#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CONTEXTS = (
    ROOT / "framegen/v3.1_src/context.cpp",
    ROOT / "framegen/v3.1p_src/context.cpp",
)


class AndroidDirectionalInputReleaseTest(unittest.TestCase):
    def test_output_copy_path_releases_direct_inputs_on_last_generated_pass(self) -> None:
        for path in CONTEXTS:
            source = path.read_text(encoding="utf-8")
            start = source.index("if (this->outputCopyRequired)")
            end = source.index("} else {", start)
            body = source[start:end]
            with self.subTest(path=path.name):
                self.assertIn("pass + 1 == generationCount", body)
                self.assertIn(
                    "add_external_release(barriers, vk, this->inImg_0",
                    body,
                )
                self.assertIn(
                    "add_external_release(barriers, vk, this->inImg_1",
                    body,
                )

    def test_release_remains_final_pass_only(self) -> None:
        for path in CONTEXTS:
            source = path.read_text(encoding="utf-8")
            start = source.index("if (this->outputCopyRequired)")
            end = source.index("} else {", start)
            body = source[start:end]
            with self.subTest(path=path.name):
                gate = body.index("pass + 1 == generationCount")
                release0 = body.index(
                    "add_external_release(barriers, vk, this->inImg_0"
                )
                release1 = body.index(
                    "add_external_release(barriers, vk, this->inImg_1"
                )
                self.assertLess(gate, release0)
                self.assertLess(gate, release1)


if __name__ == "__main__":
    unittest.main()
