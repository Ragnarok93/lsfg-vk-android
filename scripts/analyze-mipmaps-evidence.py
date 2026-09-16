#!/usr/bin/env python3
"""Analyze portable Mipmaps timing evidence with optional compiler adapters."""
from __future__ import annotations

import argparse
import json
from pathlib import Path

from mipmaps_evidence import core


def _print_text(report: dict) -> None:
    timing = report["candidate"]["timing"]
    backend = report["candidate"]["backend"]
    print(
        "candidate: mipmaps_samples={mipmaps_samples} mipmaps_avg_ms={mipmaps_avg_ms} "
        "beta4_samples={beta4_samples} beta4_avg_ms={beta4_avg_ms} backend={backend}".format(
            backend=backend["name"], **timing
        )
    )
    if "comparison" in report:
        cmp = report["comparison"]
        print(
            f"comparison: delta_ms={cmp['mipmaps_delta_ms']} "
            f"delta_percent={cmp['mipmaps_delta_percent']} verdict={cmp['verdict']} "
            f"noise_band_ms={cmp['noise_model']['noise_band_ms']} "
            f"risks={','.join(cmp['risk_flags']) or 'none'}"
        )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, action="append", default=[])
    parser.add_argument("--backend", choices=("auto", "none", "ir3"), default="auto")
    parser.add_argument("--minimum-samples", type=int, default=30)
    parser.add_argument("--noise-sigma", type=float, default=2.0)
    parser.add_argument("--minimum-improvement-percent", type=float, default=2.0)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    candidate = core.analyze_file(args.candidate, backend=args.backend)
    baselines = [core.analyze_file(path, backend=args.backend) for path in args.baseline]
    report: dict = {"candidate": candidate, "baselines": baselines}
    if len(baselines) == 1:
        report["baseline"] = baselines[0]
    if baselines:
        report["comparison"] = core.compare(
            baselines,
            candidate,
            minimum_samples=args.minimum_samples,
            noise_sigma=args.noise_sigma,
            minimum_improvement_percent=args.minimum_improvement_percent,
        )

    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        _print_text(report)


if __name__ == "__main__":
    main()
