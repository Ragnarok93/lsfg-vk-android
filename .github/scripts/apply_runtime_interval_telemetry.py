from pathlib import Path

hooks_path = Path("src/hooks.cpp")
hooks = hooks_path.read_text()


def replace_once(old: str, new: str, label: str) -> None:
    global hooks
    if old not in hooks:
        raise SystemExit(f"{label} anchor not found")
    hooks = hooks.replace(old, new, 1)


replace_once(
    "        double generatedFps{};\n        uint64_t totalSourceFrames{};",
    "        double generatedFps{};\n        uint64_t windowSourceFrames{};\n        uint64_t windowGeneratedFrames{};\n        double generatedPerSource{};\n        uint64_t totalSourceFrames{};",
    "RuntimeStatsSnapshot",
)
replace_once(
    "                << \"generated_fps=\" << snapshot.generatedFps << '\\n'\n",
    "                << \"generated_fps=\" << snapshot.generatedFps << '\\n'\n"
    "                << \"source_frames=\" << snapshot.windowSourceFrames << '\\n'\n"
    "                << \"generated_frames=\" << snapshot.windowGeneratedFrames << '\\n'\n"
    "                << \"generated_per_source=\" << snapshot.generatedPerSource << '\\n'\n",
    "stats writer",
)
replace_once(
    "                << \" output_fps=\" << snapshot.outputFps\n",
    "                << \" output_fps=\" << snapshot.outputFps\n"
    "                << \" source_frames=\" << snapshot.windowSourceFrames\n"
    "                << \" generated_frames=\" << snapshot.windowGeneratedFrames\n"
    "                << \" generated_per_source=\" << snapshot.generatedPerSource\n",
    "diagnostic writer",
)
replace_once(
    "            .generatedFps = generatedFps,\n            .totalSourceFrames = stats.totalSourceFrames,",
    "            .generatedFps = generatedFps,\n"
    "            .windowSourceFrames = stats.windowSourceFrames,\n"
    "            .windowGeneratedFrames = stats.windowGeneratedFrames,\n"
    "            .generatedPerSource = stats.windowSourceFrames > 0\n"
    "                ? static_cast<double>(stats.windowGeneratedFrames)\n"
    "                    / static_cast<double>(stats.windowSourceFrames)\n"
    "                : 0.0,\n"
    "            .totalSourceFrames = stats.totalSourceFrames,",
    "runtime snapshot queue",
)
hooks_path.write_text(hooks)

test_path = Path("tests/android_runtime_stability_test.py")
tests = test_path.read_text()
marker = "    def test_source_interval_excludes_previous_lsfg_cycle(self) -> None:\n"
new_test = '''    def test_runtime_stats_export_real_interval_generation_counts(self) -> None:
        source = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        for token in (
            "uint64_t windowSourceFrames{}",
            "uint64_t windowGeneratedFrames{}",
            "double generatedPerSource{}",
            '\"source_frames=\" << snapshot.windowSourceFrames',
            '\"generated_frames=\" << snapshot.windowGeneratedFrames',
            '\"generated_per_source=\" << snapshot.generatedPerSource',
            ".windowSourceFrames = stats.windowSourceFrames",
            ".windowGeneratedFrames = stats.windowGeneratedFrames",
        ):
            self.assertIn(token, source)

        present_start = source.index("VkResult myvkQueuePresentKHR")
        present_end = source.index("void myvkDestroySwapchainKHR", present_start)
        present = source[present_start:present_end]
        self.assertNotIn("std::ofstream", present)
        self.assertNotIn("stats.txt", present)

'''
if "test_runtime_stats_export_real_interval_generation_counts" not in tests:
    if marker not in tests:
        raise SystemExit("test insertion anchor not found")
    tests = tests.replace(marker, new_test + marker, 1)
test_path.write_text(tests)
