#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PATCHER = ROOT / "scripts/apply-candidate-b14-mipmaps-tail-fusion.py"


class B14GameNativeHelperTargetingTest(unittest.TestCase):
    @staticmethod
    def load_patcher():
        spec = importlib.util.spec_from_file_location("b14_patcher", PATCHER)
        assert spec is not None and spec.loader is not None
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def test_process_override_bypasses_wine_explorer_only(self) -> None:
        patcher = self.load_patcher()
        source = '''std::pair<std::string, std::string> Utils::getProcessName() {
    // GameNative / Wine-on-Android: /proc/self/exe points at the Wine loader,
    // not the game .exe. Accept an explicit override from the launcher so
    // per-game matching in the TOML still works.
    const char* process_exe = std::getenv("LSFG_PROCESS_EXE");
    if (process_exe && *process_exe != '\\0')
        return { process_exe, process_exe };
}
'''
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "utils.cpp"
            path.write_text(source, encoding="utf-8")
            patcher.patch_process_targeting_source(path)
            patched = path.read_text(encoding="utf-8")

            self.assertIn(patcher.PROCESS_MARKER, patched)
            self.assertIn('std::ifstream cmdline_file("/proc/self/cmdline"', patched)
            self.assertIn('constexpr char explorer_exe[] = "explorer.exe"', patched)
            self.assertIn('return { actual_process, actual_process };', patched)
            self.assertIn('return { process_exe, process_exe };', patched)

            # The patch is deliberately idempotent because GameNative's native
            # preparation can be run more than once in the same checkout.
            first = patched
            patcher.patch_process_targeting_source(path)
            self.assertEqual(first, path.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
