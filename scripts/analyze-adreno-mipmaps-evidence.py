#!/usr/bin/env python3
"""Compatibility CLI for the historical Adreno/IR3 Mipmaps evidence report."""
from __future__ import annotations

import argparse
import json
from pathlib import Path

from mipmaps_evidence import core


def legacy_view(item: dict) -> dict:
    metrics = item["backend"]["metrics"]
    return {
        **item,
        "pipeline": {
            "subgroup_sizes": item["executable"]["subgroup_sizes"],
            "statistics": item["executable"]["statistics"],
            "resident_waves": metrics.get("resident_waves"),
        },
        "ir": {
            "present": item["executable"]["present"],
            "sha256": item["executable"]["sha256"],
            "instruction_lines": metrics.get("instruction_lines", 0),
            "nop_count": metrics.get("nop_count", 0),
            "shared_load_count": metrics.get("shared_load_count", 0),
            "shared_store_count": metrics.get("shared_store_count", 0),
            "global_load_count": metrics.get("global_load_count", 0),
            "global_store_count": metrics.get("global_store_count", 0),
            "ss_count": metrics.get("ss_count", 0),
            "sy_count": metrics.get("sy_count", 0),
            "spill_markers": metrics.get("spill_markers", 0),
            "max_register_index": metrics.get("max_register_index"),
        },
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    candidate_raw = core.analyze_file(args.candidate, backend="ir3")
    candidate = legacy_view(candidate_raw)
    report: dict = {"candidate": candidate}
    if args.baseline is not None:
        baseline_raw = core.analyze_file(args.baseline, backend="ir3")
        report["baseline"] = legacy_view(baseline_raw)
        report["comparison"] = core.legacy_compare(baseline_raw, candidate_raw)

    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
        return

    timing = candidate["timing"]
    print(
        "candidate: mipmaps_samples={mipmaps_samples} mipmaps_avg_ms={mipmaps_avg_ms} "
        "beta4_samples={beta4_samples} beta4_avg_ms={beta4_avg_ms}".format(**timing)
    )
    ir = candidate["ir"]
    print(
        "candidate-ir: instructions={instruction_lines} nop={nop_count} ldl={shared_load_count} "
        "stl={shared_store_count} ss={ss_count} sy={sy_count} spills={spill_markers} "
        "max_reg={max_register_index}".format(**ir)
    )
    if "comparison" in report:
        cmp = report["comparison"]
        print(
            f"comparison: delta_ms={cmp['mipmaps_delta_ms']} "
            f"delta_percent={cmp['mipmaps_delta_percent']} verdict={cmp['verdict']} "
            f"risks={','.join(cmp['risk_flags']) or 'none'}"
        )


if __name__ == "__main__":
    main()
