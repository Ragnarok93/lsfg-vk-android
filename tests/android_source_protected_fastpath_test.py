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

# Synthetic swapchain acquisition is opportunistic in Adaptive mode. A missing
# image drops only that synthetic presentation; it must never block the source
# present thread. Fixed mode retains its existing bounded acquire behavior.
assert "generatedAcquireTimeoutNs" in source
assert "conf.adaptiveFramegen ? 0 : runtimeWaitTimeoutNs()" in compact_source
assert "res == VK_NOT_READY || res == VK_TIMEOUT" in source
assert "windowSyntheticAcquireDrops" in header
assert "lastGeneratedSourceChainSemaphore" in source
assert "presentedGeneratedFrameCount" in source

# Ring retirement is the ownership proof for framegen-side imported semaphores
# and timing resources. Reuse waits the old slot and records its completed GPU
# timing before replacing it.
for variant in ("v3.1", "v3.1p"):
    text = (ROOT / f"framegen/{variant}_src/context.cpp").read_text(encoding="utf-8")
    compact = " ".join(text.split())
    assert "if (data.shouldWait)" in text, variant
    assert "recordAdaptiveFlowGpuTiming(vk, data);" in text, variant
    assert "data.shouldWait = true;" in text, variant
    assert compact.index("recordAdaptiveFlowGpuTiming(vk, data);") < compact.index("data.shouldWait = true;"), variant

print("Source-protected Adaptive completion fast-path contract satisfied")


# Before touching either shared input AHB, Adaptive must nonblockingly prove
# that the previous framegen submission released them. Busy framegen drops only
# synthetic work and presents the real source directly; no source-time wait is
# allowed and the skipped source invalidates temporal history for a real refresh.
present_start = source.index("VkResult LsContext::present")
precopy = source.index("copySwapchainToExternalAhb", present_start)
busy_poll = source.index("previousFramegenComplete", present_start)
assert busy_poll < precopy
assert "waitContext(*this->lsfgCtxId, 0)" in compact_source
busy_block = source[busy_poll:precopy]
assert "requiresSourceHistoryWarmup_ = true" in busy_block
assert "previousSourceCopySignalValid_ = false" in busy_block
assert "gameRenderSemaphores.data()" in busy_block
assert "game-render-framegen-busy" in busy_block
assert "windowFramegenBusyBypasses" in header

# Intercepted source-only bypasses advance wrapper frameIdx but not framegen's
# temporal index, so shared-input selection must use the framegen capture index.
assert "framegenSourceFrameIdx_" in header
assert "framegenSourceFrameIdx_ % 2" in source
assert "framegenSourceFrameIdx_ < 2" in source


# Busy source-only presents do not consume an LSFG render-pass slot. Holding the
# wrapper ring index avoids reusing per-pass semaphores/command buffers merely
# because several protected source frames bypassed framegen.
assert "advanceRenderPassRingOnFinish = false" in busy_block
