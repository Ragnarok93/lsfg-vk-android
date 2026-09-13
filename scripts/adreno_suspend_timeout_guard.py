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
    metrics.windowWaitIdleMs += std::chrono::duration<double, std::milli>(
        RuntimeMetrics::Clock::now() - waitIdleStart).count();
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
    const uint64_t framegenCompletionWaitElapsedNs =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            RuntimeMetrics::Clock::now() - waitIdleStart).count());

    // SIGSTOP/SIGCONT can expire a Vulkan fence timeout while the guest is
    // suspended. Only a gross (>2x) wall-time overshoot is treated as that
    // lifecycle signature. Give resumed GPU work one tiny fresh timeout, then
    // retain the existing source-present/swapchain-recreation recovery path.
    // This is intentionally not a pacing delay and never runs in steady state.
    constexpr uint64_t resumeCompletionRecheckNs = 2'000'000ULL;
    if (!framegenReady
            && framegenCompletionWaitElapsedNs > framegenCompletionTimeoutNs * 2) {
        const auto resumeRecheckStart = RuntimeMetrics::Clock::now();
        framegenReady = waitFramegenCompletion(resumeCompletionRecheckNs);
        const double resumeRecheckMs = std::chrono::duration<double, std::milli>(
            RuntimeMetrics::Clock::now() - resumeRecheckStart).count();
        std::cerr << "lsfg-vk: runtime stage=framegen-completion-resume-recheck"
                  << " recovered=" << (framegenReady ? 1 : 0)
                  << " overshoot_ms="
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
