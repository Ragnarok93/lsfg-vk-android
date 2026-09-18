#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")

# Source pacing is indexed only by real source arrivals. The removed call-count
# accumulator must never return.
assert "SourceFrameTimeline sourceTimeline_" in header
assert "adaptiveNextPresentTimeNs_" not in header
assert "adaptiveNextPresentTimeNs_" not in source
assert "sourceTimeline_.observe(" in source
assert "adaptiveScheduler_.planSlots(sourceInterval)" in source

# Adaptive fractional output must carry the scheduler's exact interpolation
# phases into framegen rather than translating them back to integer counts.
assert "presentContextWithPhases" in source
assert "adaptivePlan.slotPhases" in source
assert "presentContextWithCount" in source  # fixed/history compatibility remains

# VK_GOOGLE_display_timing is advisory only. Desired times are explicit inputs
# derived from one source cycle and stale/historical targets are omitted.
assert "desiredPresentTimeNs" in source
assert "sourceCycle.sourceDeadlineNs" in source
assert "sourceCycle.syntheticDeadlineNs" in source
assert "desiredPresentTimeNs <= nowNs" in source

# No source-pacing sleeps or generated-call-count rephasing.
assert "sleep_until" not in source
assert "delayUntilNextSourceOutput" not in source

print("Source-protected presentation timing contract satisfied")
