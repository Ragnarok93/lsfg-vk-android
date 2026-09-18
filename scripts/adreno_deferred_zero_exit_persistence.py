#!/usr/bin/env python3
"""Require persistent scheduler demand before waking DeferredZero history.

AdaptiveFrameScheduler remains authoritative and unchanged. Its fractional
budget can legitimately emit isolated generated-frame requests while the source
hovers just below the target. Paying the three-present live re-prime cost for a
single sparse request is counterproductive, so DeferredZero treats generated
requests as wake evidence: two requests inside a 500 ms window are required
before entering the already-validated ReprimeHistory live-warmup path.

Requests that do not satisfy the density gate remain on the game-device-local
source-only raw ring. Once the gate opens, the existing three-frame live re-prime
runs unchanged; no generated-frame backlog is replayed or burst.
"""
from __future__ import annotations

from pathlib import Path

from adreno_evidence_common import replace_exact


def once(text: str, old: str, new: str, label: str) -> str:
    return replace_exact(text, old, new, count=1, label=label)


def patch_header(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "kDeferredZeroWakeRequestThreshold = 2" in text:
        return

    text = once(
        text,
        "    static constexpr size_t kDeferredZeroWarmupFrames = 3;\n"
        "    size_t deferredZeroWarmupFramesRemaining_{0};\n"
        "    std::chrono::steady_clock::time_point deferredReprimeStart_{};\n",
        "    static constexpr size_t kDeferredZeroWarmupFrames = 3;\n"
        "    static constexpr size_t kDeferredZeroWakeRequestThreshold = 2;\n"
        "    static constexpr uint64_t kDeferredZeroWakeRequestWindowMs = 500;\n"
        "    size_t deferredZeroWarmupFramesRemaining_{0};\n"
        "    std::chrono::steady_clock::time_point deferredReprimeStart_{};\n"
        "    size_t deferredZeroWakeRequestCount_{0};\n"
        "    std::chrono::steady_clock::time_point deferredZeroWakeWindowStart_{};\n",
        f"{path}: DeferredZero wake-density state",
    )
    path.write_text(text, encoding="utf-8")


def patch_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "deferred-zero reprime-deferred" in text:
        return

    # Any lifecycle invalidation cancels a partially accumulated wake window.
    invalidate_start_marker = "void LsContext::invalidateDeferredZeroHistory() {\n"
    ordered_slots_marker = "std::array<size_t, 3> LsContext::orderedRawHistorySlots() const {\n"
    invalidate_start = text.find(invalidate_start_marker)
    ordered_slots_start = text.find(ordered_slots_marker, invalidate_start)
    if invalidate_start < 0 or ordered_slots_start < 0:
        raise RuntimeError(f"{path}: DeferredZero lifecycle method boundaries not found")
    invalidate_block = text[invalidate_start:ordered_slots_start]
    reset_anchor = (
        "    this->deferredZeroWarmupFramesRemaining_ = 0;\n"
        "    this->deferredReprimeStart_ = {};\n"
    )
    if invalidate_block.count(reset_anchor) != 1:
        raise RuntimeError(
            f"{path}: safe re-prime lifecycle reset anchor expected once, "
            f"found {invalidate_block.count(reset_anchor)}"
        )
    invalidate_block = invalidate_block.replace(
        reset_anchor,
        reset_anchor
        + "    this->deferredZeroWakeRequestCount_ = 0;\n"
        + "    this->deferredZeroWakeWindowStart_ = {};\n",
        1,
    )
    text = text[:invalidate_start] + invalidate_block + text[ordered_slots_start:]

    # Fresh DeferredZero entries always begin with an empty wake-evidence window.
    text = once(
        text,
        "            this->historyMaintenanceState_ = HistoryMaintenanceState::DeferredZero;\n"
        "            metrics.windowDeferredZeroEntries++;\n",
        "            this->historyMaintenanceState_ = HistoryMaintenanceState::DeferredZero;\n"
        "            this->deferredZeroWakeRequestCount_ = 0;\n"
        "            this->deferredZeroWakeWindowStart_ = {};\n"
        "            metrics.windowDeferredZeroEntries++;\n",
        f"{path}: reset wake-density state on DeferredZero entry",
    )

    # Replace the safe re-prime's immediate wake with a density gate. The
    # scheduler's discrete generatedFrameCount tokens are only wake evidence;
    # we never modify AdaptiveFrameScheduler or manufacture backlog.
    old_transition = r'''        if (this->historyMaintenanceState_ == HistoryMaintenanceState::DeferredZero) {
            this->historyMaintenanceState_ = HistoryMaintenanceState::ReprimeHistory;
            this->deferredZeroWarmupFramesRemaining_ = kDeferredZeroWarmupFrames;
            this->deferredReprimeStart_ = cycleStart;
            std::cerr << "lsfg-vk: deferred-zero reprime-warmup-begin frames="
                      << this->deferredZeroWarmupFramesRemaining_ << '\n';
        } else if (this->historyMaintenanceState_ == HistoryMaintenanceState::LiveHistory) {
'''
    new_transition = r'''        const bool deferredZeroWakeRequested = generatedFrameCount > 0;
        if (this->historyMaintenanceState_ == HistoryMaintenanceState::DeferredZero
                && deferredZeroWakeRequested) {
            const bool wakeWindowActive = this->deferredZeroWakeRequestCount_ > 0;
            const uint64_t wakeWindowMs = wakeWindowActive
                ? static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    cycleStart - this->deferredZeroWakeWindowStart_).count())
                : 0;
            if (!wakeWindowActive || wakeWindowMs > kDeferredZeroWakeRequestWindowMs) {
                this->deferredZeroWakeRequestCount_ = 1;
                this->deferredZeroWakeWindowStart_ = cycleStart;
            } else {
                ++this->deferredZeroWakeRequestCount_;
            }

            if (this->deferredZeroWakeRequestCount_ >= kDeferredZeroWakeRequestThreshold) {
                this->deferredZeroWakeRequestCount_ = 0;
                this->deferredZeroWakeWindowStart_ = {};
                this->historyMaintenanceState_ = HistoryMaintenanceState::ReprimeHistory;
                this->deferredZeroWarmupFramesRemaining_ = kDeferredZeroWarmupFrames;
                this->deferredReprimeStart_ = cycleStart;
                std::cerr << "lsfg-vk: deferred-zero reprime-warmup-begin frames="
                          << this->deferredZeroWarmupFramesRemaining_ << '\n';
            } else {
                std::cerr << "lsfg-vk: deferred-zero reprime-deferred request_count="
                          << this->deferredZeroWakeRequestCount_
                          << " window_ms=" << wakeWindowMs
                          << " threshold=" << kDeferredZeroWakeRequestThreshold << '\n';
            }
        } else if (this->historyMaintenanceState_ == HistoryMaintenanceState::LiveHistory) {
            this->deferredZeroWakeRequestCount_ = 0;
            this->deferredZeroWakeWindowStart_ = {};
'''
    text = once(
        text,
        old_transition,
        new_transition,
        f"{path}: gate DeferredZero exit on dense scheduler requests",
    )

    # While a first isolated scheduler token is waiting for confirmation, stay
    # on the already-validated source-only raw-ring path instead of entering the
    # normal framegen path with invalid history.
    text = once(
        text,
        "    if (this->historyMaintenanceState_ == HistoryMaintenanceState::DeferredZero\n"
        "            && adaptiveZeroGeneration) {\n",
        "    if (this->historyMaintenanceState_ == HistoryMaintenanceState::DeferredZero) {\n",
        f"{path}: keep pending wake requests source-only",
    )

    # A deferred non-zero scheduler request is not an adaptive-zero decision.
    # Keep the existing zero-cycle telemetry honest while still counting every
    # source-only DeferredZero frame.
    text = once(
        text,
        "        metrics.windowAdaptiveZeroGenerationCycles++;\n"
        "        metrics.totalAdaptiveZeroGenerationCycles++;\n"
        "        metrics.windowDeferredZeroFrames++;\n",
        "        if (adaptiveZeroGeneration) {\n"
        "            metrics.windowAdaptiveZeroGenerationCycles++;\n"
        "            metrics.totalAdaptiveZeroGenerationCycles++;\n"
        "        }\n"
        "        metrics.windowDeferredZeroFrames++;\n",
        f"{path}: preserve zero-cycle telemetry under deferred wake requests",
    )

    path.write_text(text, encoding="utf-8")


def apply(root: Path) -> None:
    patch_header(root / "include/context.hpp")
    patch_source(root / "src/context.cpp")


def main() -> None:
    import argparse

    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    apply(args.root.resolve())


if __name__ == "__main__":
    main()
