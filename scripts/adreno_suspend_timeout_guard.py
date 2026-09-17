#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path

from adreno_evidence_common import replace_exact


OLD_WAIT = '''    const auto waitIdleStart = RuntimeMetrics::Clock::now();
    const uint64_t framegenCompletionTimeoutNs = runtimeWaitTimeoutNs();
    const bool framegenReady = conf.performance
        ? LSFG_3_1P::waitContext(*this->lsfgCtxId, framegenCompletionTimeoutNs)
        : LSFG_3_1::waitContext(*this->lsfgCtxId, framegenCompletionTimeoutNs);
    if (!framegenRecoveredAfterTimeout) {
        metrics.windowWaitIdleMs += std::chrono::duration<double, std::milli>(
            RuntimeMetrics::Clock::now() - waitIdleStart).count();
    } else {
        // A SIGSTOP/SIGCONT recovery is lifecycle time, not framegen cost.
        excludeCurrentCycleFromTimingMetrics = true;
    }
    if (!framegenReady) {
'''

NEW_WAIT = '''    const auto waitIdleStart = RuntimeMetrics::Clock::now();
    const uint64_t framegenCompletionTimeoutNs = runtimeWaitTimeoutNs();
    const auto waitFramegenCompletion = [&](uint64_t timeoutNs) {
        return conf.performance
            ? LSFG_3_1P::waitContext(*this->lsfgCtxId, timeoutNs)
            : LSFG_3_1::waitContext(*this->lsfgCtxId, timeoutNs);
    };
    bool framegenReady = waitFramegenCompletion(framegenCompletionTimeoutNs);
    bool framegenRecoveredAfterTimeout = false;
    const uint64_t framegenCompletionWaitElapsedNs =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            RuntimeMetrics::Clock::now() - waitIdleStart).count());

    // SIGSTOP/SIGCONT can leave the first bounded fence wait stale even when
    // resumed GPU work is healthy. Any timeout gets exactly one fresh bounded
    // wait. This is an error/lifecycle recovery path only: it never executes
    // during steady-state pacing and does not change the normal timeout budget.
    constexpr uint64_t resumeCompletionRecheckNs = 32'000'000ULL;
    if (!framegenReady) {
        const auto resumeRecheckStart = RuntimeMetrics::Clock::now();
        framegenReady = waitFramegenCompletion(resumeCompletionRecheckNs);
        framegenRecoveredAfterTimeout = framegenReady;
        const double resumeRecheckMs = std::chrono::duration<double, std::milli>(
            RuntimeMetrics::Clock::now() - resumeRecheckStart).count();
        std::cerr << "lsfg-vk: runtime stage=framegen-completion-resume-recheck"
                  << " recovered=" << (framegenReady ? 1 : 0)
                  << " initial_wait_ms="
                  << (static_cast<double>(framegenCompletionWaitElapsedNs) / 1000000.0)
                  << " recheck_ms=" << resumeRecheckMs << "\\n";
    }

    metrics.windowWaitIdleMs += std::chrono::duration<double, std::milli>(
        RuntimeMetrics::Clock::now() - waitIdleStart).count();
    if (!framegenReady) {
'''


def apply(root: Path) -> None:
    path = root / "src/context.cpp"
    text = path.read_text(encoding="utf-8")
    if "framegenCompletionWaitElapsedNs" in text:
        return
    text = replace_exact(
        text,
        OLD_WAIT,
        NEW_WAIT,
        count=1,
        label=f"{path}: suspend-safe framegen completion wait",
    )
    path.write_text(text, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    apply(args.root.resolve())


if __name__ == "__main__":
    main()
