#!/usr/bin/env python3
"""Finalize Candidate A checkpoint telemetry and source-present fidelity."""
from __future__ import annotations

import argparse
from pathlib import Path

from adreno_evidence_common import replace_exact


def once(text: str, old: str, new: str, label: str) -> str:
    return replace_exact(text, old, new, count=1, label=label)


def patch_header(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "windowDeferredReprimeMs" in text:
        return
    text = once(
        text,
        "        uint64_t windowDeferredReprimes{0};\n"
        "        uint64_t totalDeferredReprimes{0};\n"
        "        uint64_t totalDeferredFallbacks{0};\n",
        "        uint64_t windowDeferredReprimes{0};\n"
        "        uint64_t totalDeferredReprimes{0};\n"
        "        double windowDeferredReprimeMs{0.0};\n"
        "        double totalDeferredReprimeMs{0.0};\n"
        "        uint64_t totalDeferredFallbacks{0};\n",
        f"{path}: re-prime timing metrics",
    )
    path.write_text(text, encoding="utf-8")


def patch_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "deferred_zero_reprime_time_ms=" in text:
        return

    text = once(
        text,
        "        return finishSourcePresent(returnedResult, waitLabel);\n"
        "    };\n\n"
        "    // deferred-zero reprime-begin\n",
        "        const VkResult finishResult = returnedResult == VK_ERROR_OUT_OF_DATE_KHR\n"
        "            ? returnedResult : result;\n"
        "        return finishSourcePresent(finishResult, waitLabel);\n"
        "    };\n\n"
        "    // deferred-zero reprime-begin\n",
        f"{path}: preserve successful present result",
    )

    text = once(
        text,
        "    if (this->historyMaintenanceState_ == HistoryMaintenanceState::ReprimeHistory\n"
        "            && generatedFrameCount > 0) {\n"
        "        std::cerr << \"lsfg-vk: deferred-zero reprime-begin history_count=\"\n",
        "    if (this->historyMaintenanceState_ == HistoryMaintenanceState::ReprimeHistory\n"
        "            && generatedFrameCount > 0) {\n"
        "        const auto deferredReprimeStart = RuntimeMetrics::Clock::now();\n"
        "        std::cerr << \"lsfg-vk: deferred-zero reprime-begin history_count=\"\n",
        f"{path}: re-prime timing start",
    )

    text = once(
        text,
        "        this->lastGeneratedFrameCount_ = 0;\n"
        "        if (!reprimeSucceeded)\n",
        "        const double deferredReprimeMs = std::chrono::duration<double, std::milli>(\n"
        "            RuntimeMetrics::Clock::now() - deferredReprimeStart).count();\n"
        "        metrics.windowDeferredReprimeMs += deferredReprimeMs;\n"
        "        metrics.totalDeferredReprimeMs += deferredReprimeMs;\n"
        "        this->lastGeneratedFrameCount_ = 0;\n"
        "        if (!reprimeSucceeded)\n",
        f"{path}: re-prime timing finish",
    )

    text = once(
        text,
        "        std::cerr << \"lsfg-vk: deferred-zero reprime-complete source_only=1\\n\";\n",
        "        std::cerr << \"lsfg-vk: deferred-zero reprime-complete source_only=1 reprime_ms=\"\n"
        "                  << deferredReprimeMs << '\\n';\n",
        f"{path}: re-prime completion metric",
    )

    text = once(
        text,
        "                      << \" deferred_zero_reprime_total=\" << metrics.totalDeferredReprimes\n"
        "                      << \" deferred_zero_fallbacks_total=\" << metrics.totalDeferredFallbacks\n",
        "                      << \" deferred_zero_reprime_total=\" << metrics.totalDeferredReprimes\n"
        "                      << \" deferred_zero_reprime_time_ms=\" << metrics.windowDeferredReprimeMs\n"
        "                      << \" deferred_zero_reprime_time_ms_total=\" << metrics.totalDeferredReprimeMs\n"
        "                      << \" deferred_zero_fallbacks_total=\" << metrics.totalDeferredFallbacks\n",
        f"{path}: re-prime runtime metrics",
    )

    text = once(
        text,
        "            metrics.windowDeferredReprimes = 0;\n",
        "            metrics.windowDeferredReprimes = 0;\n"
        "            metrics.windowDeferredReprimeMs = 0.0;\n",
        f"{path}: re-prime metric reset",
    )
    path.write_text(text, encoding="utf-8")


def apply(root: Path) -> None:
    patch_header(root / "include/context.hpp")
    patch_source(root / "src/context.cpp")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    apply(args.root.resolve())


if __name__ == "__main__":
    main()
