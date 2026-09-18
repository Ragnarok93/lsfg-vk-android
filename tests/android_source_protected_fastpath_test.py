#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
compact_source = " ".join(source.split())
header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")

# Adaptive source protection requires both halves of the cross-device handoff:
# input copy -> framegen and framegen completion -> game post-copy. The latter
# removes the per-generated-cycle CPU completion wait after calibration.
assert "asyncFramegenCompletionEnabled_" in header
assert "renderSemaphoreFds" in source
assert "pass.renderSemaphores.at(i) = Mini::Semaphore" in compact_source
assert "presentContextWithPhases" in source
assert "renderSemaphoreFds : noOutSems, interpolationPhases" in compact_source
assert "pass.renderSemaphores.at(i).handle()" in source
assert "useAsyncFramegenCompletion" in source

# Deadline admission must be active for Adaptive work, not telemetry-only.
assert "deadlineAdmissionPlan" in source
assert "admittedPhases" in source
assert "slot.admitted" in source
assert "interpolationPhases = std::move(admittedPhases)" in source
assert "generatedFrameCount = interpolationPhases.size()" in source
assert "deadline-shadow" not in source

# The old bounded host wait remains as a capability/calibration fallback and
# for fixed mode; it must not be the normal Adaptive completion path.
assert "waitContext" in source
assert "if (!useAsyncFramegenCompletion)" in source

# Ring retirement is the ownership proof for framegen-side imported semaphores
# and timing resources. Reuse waits the old slot and records its completed GPU
# timing before replacing it.
for variant in ("v3.1", "v3.1p"):
    text = (ROOT / f"framegen/{variant}_src/context.cpp").read_text(encoding="utf-8")
    compact = " ".join(text.split())
    assert "if (data.shouldWait)" in text, variant
    assert "recordAdaptiveFlowGpuTiming(vk, data);" in text, variant
    assert compact.index("recordAdaptiveFlowGpuTiming(vk, data);") < compact.index("data.shouldWait = generationCount > 0;"), variant

print("Source-protected Adaptive completion fast-path contract satisfied")
