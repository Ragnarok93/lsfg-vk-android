#!/usr/bin/env python3
"""Replace DeferredZero's synchronous retained-frame replay with safe live warmup.

The retained game-local raw ring remains useful for the steady DeferredZero fast
path, but Android device logs show that replaying those retained images back
through the shared AHB boundary can tear down the private framegen instance.
On exit, keep presenting real source frames and run the already-proven normal
zero-generation history-maintenance path for three cycles.  Generation resumes
only after that live history is rebuilt.
"""
from __future__ import annotations

from pathlib import Path

from adreno_evidence_common import replace_exact


def once(text: str, old: str, new: str, label: str) -> str:
    return replace_exact(text, old, new, count=1, label=label)


def patch_header(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "kDeferredZeroWarmupFrames = 3" in text:
        return

    text = once(
        text,
        "    uint64_t framegenHistoryEpoch_{0};\n",
        "    uint64_t framegenHistoryEpoch_{0};\n"
        "    static constexpr size_t kDeferredZeroWarmupFrames = 3;\n"
        "    size_t deferredZeroWarmupFramesRemaining_{0};\n"
        "    std::chrono::steady_clock::time_point deferredReprimeStart_{};\n",
        f"{path}: safe DeferredZero warmup state",
    )
    path.write_text(text, encoding="utf-8")


def patch_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "deferred-zero reprime-warmup-begin" in text:
        return

    # The raw ring is game-device-local. Keep the source-capture helper, but
    # remove the now-unused retained-frame -> shared-AHB replay helper entirely.
    raw_replay_helper_marker = (
        "// Re-prime copies one retained game-local source into the existing framegen AHB.\n"
    )
    normal_history_marker = (
        "// Acquire a generated AHB from framegen, copy it into an acquired swapchain\n"
    )
    helper_start = text.find(raw_replay_helper_marker)
    helper_end = text.find(normal_history_marker, helper_start)
    if helper_start < 0 or helper_end < 0:
        raise RuntimeError(f"{path}: DeferredZero raw replay helper markers not found")
    text = text[:helper_start] + text[helper_end:]

    # Lifecycle resets must also cancel any in-progress live warmup.
    text = once(
        text,
        "    this->zeroDemandStart_ = {};\n}\n#endif\n\n",
        "    this->zeroDemandStart_ = {};\n"
        "    this->deferredZeroWarmupFramesRemaining_ = 0;\n"
        "    this->deferredReprimeStart_ = {};\n"
        "}\n#endif\n\n",
        f"{path}: reset safe DeferredZero warmup",
    )

    # When demand for generated frames returns, do not synchronously replay the
    # retained ring. Start a three-present live-history warmup instead.
    old_transition = '''        if (this->historyMaintenanceState_ == HistoryMaintenanceState::DeferredZero) {
            this->historyMaintenanceState_ = HistoryMaintenanceState::ReprimeHistory;
        } else if (this->historyMaintenanceState_ == HistoryMaintenanceState::LiveHistory) {
'''
    new_transition = '''        if (this->historyMaintenanceState_ == HistoryMaintenanceState::DeferredZero) {
            this->historyMaintenanceState_ = HistoryMaintenanceState::ReprimeHistory;
            this->deferredZeroWarmupFramesRemaining_ = kDeferredZeroWarmupFrames;
            this->deferredReprimeStart_ = cycleStart;
            std::cerr << "lsfg-vk: deferred-zero reprime-warmup-begin frames="
                      << this->deferredZeroWarmupFramesRemaining_ << '\n';
        } else if (this->historyMaintenanceState_ == HistoryMaintenanceState::LiveHistory) {
'''
    text = once(text, old_transition, new_transition, f"{path}: start live DeferredZero warmup")

    # All legacy replay variants are bounded by these two stable comments. They
    # may have accumulated SYNC_FD, guard, and timing edits before this final
    # transform; replacing by markers keeps this fix independent of those details.
    reprime_start_marker = "    // deferred-zero reprime-begin\n"
    normal_history_marker = (
        "    // 1. Copy every active Adaptive source frame into frame_0/frame_1, even on\n"
    )
    reprime_start = text.find(reprime_start_marker)
    normal_history_start = text.find(normal_history_marker, reprime_start)
    if reprime_start < 0 or normal_history_start < 0:
        raise RuntimeError(f"{path}: DeferredZero reprime block markers not found")
    safe_gate = '''    const bool deferredReprimeWarmup =
        this->historyMaintenanceState_ == HistoryMaintenanceState::ReprimeHistory
        && this->deferredZeroWarmupFramesRemaining_ > 0;

'''
    text = text[:reprime_start] + safe_gate + text[normal_history_start:]

    # Live re-prime uses the same async game->framegen SYNC_FD handoff as the
    # established adaptive-zero path, even when the scheduler currently asks
    # for generated frames.
    text = once(
        text,
        "    bool useAsyncHandoff = this->asyncAhbHandoffEnabled_ && !warmupSourceHistory && (generatedFrameCount>0 || (adaptiveZeroGeneration && this->asyncZeroHistoryEnabled_));\n",
        "    bool useAsyncHandoff = this->asyncAhbHandoffEnabled_ && !warmupSourceHistory && "
        "(generatedFrameCount>0 || ((adaptiveZeroGeneration || deferredReprimeWarmup) "
        "&& this->asyncZeroHistoryEnabled_));\n",
        f"{path}: allow async handoff during safe DeferredZero warmup",
    )

    zero_branch_marker = '''    if (adaptiveZeroGeneration) {
        // The framegen zero-count path advances its temporal frame index without
'''
    text = once(
        text,
        zero_branch_marker,
        '''    if (adaptiveZeroGeneration || deferredReprimeWarmup) {
        // The framegen zero-count path advances its temporal frame index without
''',
        f"{path}: route DeferredZero warmup through normal zero-history maintenance",
    )

    old_counters = '''        this->framegenHistoryEpoch_++;
        metrics.windowAdaptiveZeroGenerationCycles++;
        metrics.totalAdaptiveZeroGenerationCycles++;
        this->requiresSourceHistoryWarmup_ = false;
'''
    new_counters = '''        this->framegenHistoryEpoch_++;
        if (adaptiveZeroGeneration) {
            metrics.windowAdaptiveZeroGenerationCycles++;
            metrics.totalAdaptiveZeroGenerationCycles++;
        }
        if (deferredReprimeWarmup) {
            if (this->deferredZeroWarmupFramesRemaining_ > 0)
                --this->deferredZeroWarmupFramesRemaining_;
            if (this->deferredZeroWarmupFramesRemaining_ == 0) {
                const double deferredReprimeMs = std::chrono::duration<double, std::milli>(
                    RuntimeMetrics::Clock::now() - this->deferredReprimeStart_).count();
                metrics.windowDeferredReprimeMs += deferredReprimeMs;
                metrics.totalDeferredReprimeMs += deferredReprimeMs;
                metrics.windowDeferredReprimes++;
                metrics.totalDeferredReprimes++;
                this->historyMaintenanceState_ = HistoryMaintenanceState::LiveHistory;
                this->rawSourceHistoryNext_ = 0;
                this->rawSourceHistoryCount_ = 0;
                this->rawSourceHistoryInitialized_.fill(false);
                this->deferredReprimeStart_ = {};
                std::cerr << "lsfg-vk: deferred-zero reprime-complete source_only=1 reprime_ms="
                          << deferredReprimeMs << '\n';
            } else {
                std::cerr << "lsfg-vk: deferred-zero reprime-warmup-progress remaining="
                          << this->deferredZeroWarmupFramesRemaining_ << '\n';
            }
        }
        this->requiresSourceHistoryWarmup_ = false;
'''
    text = once(
        text,
        old_counters,
        new_counters,
        f"{path}: count safe DeferredZero live warmup",
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
