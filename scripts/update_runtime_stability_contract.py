from pathlib import Path

path = Path("tests/android_runtime_stability_test.py")
text = path.read_text(encoding="utf-8")
old = '''    def test_generated_and_source_presents_are_output_cadence_spaced(self) -> None:
        """Mailbox/high-refresh presents are phase-spaced instead of queued in a burst."""
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        frame_pacer = (ROOT / "include/frame_pacer.hpp").read_text(encoding="utf-8")

        self.assertIn('#include "output_frame_pacer.hpp"', header)
        self.assertIn("OutputFramePacer outputFramePacer_", header)
        self.assertIn("outputFramePacer_.configure(conf.fpsLimit)", source)
        self.assertIn("periodRemainder_", frame_pacer)
        self.assertIn("phaseRemainder_", frame_pacer)
        self.assertIn("waitForFineOutputDeadline", source)
        generated_present = source.index("Layer::ovkQueuePresentKHR(queue, &presentInfo)")
        generated_pacing = source.rfind("paceOutputPresent", 0, generated_present)
        source_present = source.index("Layer::ovkQueuePresentKHR(queue, &finalPresentInfo)")
        source_pacing = source.rfind("paceOutputPresent", 0, source_present)
        self.assertGreater(generated_pacing, source.index("for (size_t i = 0; i < generatedFrameCount"))
        self.assertGreater(source_pacing, generated_present)
'''
new = '''    def test_generated_and_source_presents_are_output_cadence_spaced(self) -> None:
        """Phase spacing remains, but its cadence is owned by the correct mode."""
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        frame_pacer = (ROOT / "include/frame_pacer.hpp").read_text(encoding="utf-8")
        policy = (ROOT / "include/runtime_policy.hpp").read_text(encoding="utf-8")

        self.assertIn('#include "output_frame_pacer.hpp"', header)
        self.assertIn('#include "runtime_policy.hpp"', source)
        self.assertIn("OutputFramePacer outputFramePacer_", header)
        self.assertIn("resolveOutputPacingTarget(", source)
        self.assertIn("conf.adaptiveFramegen, conf.fpsLimit", source)
        self.assertIn("conf.sourceFpsLimit, conf.multiplier", source)
        self.assertIn("outputFramePacer_.configure(outputPacingTarget)", source)
        self.assertNotIn("outputFramePacer_.configure(conf.fpsLimit)", source)
        self.assertIn("capacityFps = saturatingOutputRate(sourceFpsLimit, multiplier)", policy)
        self.assertIn("return std::min(adaptiveTargetFps, capacityFps)", policy)
        self.assertIn("periodRemainder_", frame_pacer)
        self.assertIn("phaseRemainder_", frame_pacer)
        self.assertIn("waitForFineOutputDeadline", source)
        generated_present = source.index("Layer::ovkQueuePresentKHR(queue, &presentInfo)")
        generated_pacing = source.rfind("paceOutputPresent", 0, generated_present)
        source_present = source.index("Layer::ovkQueuePresentKHR(queue, &finalPresentInfo)")
        source_pacing = source.rfind("paceOutputPresent", 0, source_present)
        self.assertGreater(generated_pacing, source.index("for (size_t i = 0; i < generatedFrameCount"))
        self.assertGreater(source_pacing, generated_present)
'''
if text.count(old) != 1:
    raise SystemExit(f"expected one pacing contract block, found {text.count(old)}")
path.write_text(text.replace(old, new, 1), encoding="utf-8")

for helper in (
    Path("scripts/update_runtime_stability_contract.py"),
    Path(".github/workflows/apply-runtime-contract.yml"),
):
    if helper.exists():
        helper.unlink()
