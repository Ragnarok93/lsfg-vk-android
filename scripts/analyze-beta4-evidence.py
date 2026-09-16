#!/usr/bin/env python3
"""Analyze B12 timing plus final p_beta[4] executable evidence."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import statistics
from pathlib import Path
from typing import Any

from mipmaps_evidence import core
from mipmaps_evidence.backends import ir3


SHADER = "p_beta[4]"
PROPERTY_RE = re.compile(
    r"pipeline-exec-property shader=p_beta\[4\] executable=(?P<exec>\d+).*?"
    r"subgroup_size=(?P<subgroup>\d+).*?name=\"(?P<name>(?:\\.|[^\"])*)\""
)
STAT_RE = re.compile(
    r"pipeline-exec-stat shader=p_beta\[4\] executable=(?P<exec>\d+) "
    r"stat=(?P<stat>\d+) format=(?P<format>\S+) value=(?P<value>\S+) "
    r'name="(?P<name>(?:\\.|[^"])*)"'
)
IR_RE = re.compile(
    r"pipeline-exec-ir-line shader=p_beta\[4\] executable=(?P<exec>\d+) "
    r"ir=(?P<ir>\d+) line=(?P<line>\d+) chunk=(?P<chunk>\d+) "
    r"chunks=(?P<chunks>\d+) text=\"(?P<text>.*)\"$"
)


def unescape(text: str) -> str:
    output: list[str] = []
    index = 0
    while index < len(text):
        if text[index] != "\\" or index + 1 >= len(text):
            output.append(text[index])
            index += 1
            continue
        nxt = text[index + 1]
        output.append({"n": "\n", "r": "\r", "t": "\t"}.get(nxt, nxt))
        index += 2
    return "".join(output)


def number(value: str) -> int | float | str:
    try:
        return int(value, 0)
    except ValueError:
        try:
            return float(value)
        except ValueError:
            return value


def stat_name(name: str) -> str:
    return re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_") or "unnamed"


def analyze_text(text: str) -> dict[str, Any]:
    portable = core.analyze_text(text, backend="none")
    executable_names: list[str] = []
    subgroup_sizes: list[int] = []
    stats: dict[str, int | float | str] = {}
    chunks: dict[tuple[int, int, int], dict[int, str]] = {}

    for raw_line in text.splitlines():
        prop = PROPERTY_RE.search(raw_line)
        if prop:
            executable_names.append(unescape(prop.group("name")))
            subgroup_sizes.append(int(prop.group("subgroup")))

        stat = STAT_RE.search(raw_line)
        if stat:
            stats[stat_name(unescape(stat.group("name")))] = number(stat.group("value"))

        ir_match = IR_RE.search(raw_line)
        if ir_match:
            key = (
                int(ir_match.group("exec")),
                int(ir_match.group("ir")),
                int(ir_match.group("line")),
            )
            chunks.setdefault(key, {})[int(ir_match.group("chunk"))] = unescape(
                ir_match.group("text")
            )

    lines = ["".join(chunks[key][part] for part in sorted(chunks[key])) for key in sorted(chunks)]
    ir_text = "\n".join(lines)
    executable = {
        "present": bool(lines or executable_names),
        "sha256": hashlib.sha256(ir_text.encode("utf-8")).hexdigest() if lines else None,
        "line_count": len(lines),
        "names": sorted(set(executable_names)),
        "subgroup_sizes": sorted(set(subgroup_sizes)),
        "statistics": stats,
    }
    backend_name = "ir3" if ir3.supports(executable_names, lines) else (
        "unsupported" if executable["present"] else "none"
    )
    metrics = ir3.analyze(lines, stats) if backend_name == "ir3" else {}
    return {
        "shader": SHADER,
        "device": portable["device"],
        "timing": portable["timing"],
        "executable": executable,
        "backend": {
            "name": backend_name,
            "available": backend_name == "ir3",
            "metrics": metrics,
        },
    }


def analyze_file(path: Path) -> dict[str, Any]:
    return analyze_text(path.read_text(encoding="utf-8", errors="replace"))


def compare(
    baselines: list[dict[str, Any]],
    candidate: dict[str, Any],
    *,
    minimum_samples: int,
    noise_sigma: float,
    minimum_improvement_percent: float,
) -> dict[str, Any]:
    baseline_samples = sum(item["timing"]["beta4_samples"] for item in baselines)
    weighted = sum(
        item["timing"]["beta4_samples"] * item["timing"]["beta4_avg_ms"]
        for item in baselines
        if item["timing"]["beta4_avg_ms"] is not None
    )
    baseline_ms = weighted / baseline_samples if baseline_samples else None
    candidate_ms = candidate["timing"]["beta4_avg_ms"]
    means = [
        item["timing"]["beta4_avg_ms"]
        for item in baselines
        if item["timing"]["beta4_avg_ms"] is not None
    ]
    stdev_ms = statistics.stdev(means) if len(means) >= 2 else None
    noise_band_ms = noise_sigma * stdev_ms if stdev_ms is not None else None
    delta_ms = candidate_ms - baseline_ms if candidate_ms is not None and baseline_ms is not None else None
    delta_percent = (delta_ms / baseline_ms) * 100.0 if delta_ms is not None and baseline_ms else None

    baseline_hashes = {
        item["executable"]["sha256"] for item in baselines if item["executable"]["sha256"]
    }
    candidate_hash = candidate["executable"]["sha256"]
    executable_changed = (
        candidate_hash not in baseline_hashes
        if len(baseline_hashes) == 1 and candidate_hash is not None
        else None
    )

    risks: list[str] = []
    if baselines and candidate["backend"]["name"] == "ir3":
        reference = baselines[0]
        if reference["backend"]["name"] == "ir3":
            risks = ir3.compare(reference["backend"]["metrics"], candidate["backend"]["metrics"])

    insufficient: list[str] = []
    if len(means) < 2:
        insufficient.append("baseline_noise_model")
    if candidate["timing"]["beta4_samples"] < minimum_samples:
        insufficient.append("candidate_samples")
    if delta_ms is None:
        insufficient.append("timing")
    if executable_changed is None:
        insufficient.append("executable")

    if risks:
        verdict = "reject"
    elif insufficient:
        verdict = "insufficient"
    elif delta_ms is None or delta_percent is None or noise_band_ms is None:
        verdict = "insufficient"
    elif (
        delta_ms < -noise_band_ms
        and delta_percent <= -minimum_improvement_percent
        and executable_changed is True
    ):
        verdict = "promising"
    elif delta_ms > noise_band_ms and delta_percent >= minimum_improvement_percent:
        verdict = "regression"
    elif abs(delta_ms) <= noise_band_ms or abs(delta_percent) < minimum_improvement_percent:
        verdict = "noise"
    else:
        verdict = "inconclusive"

    return {
        "baseline_beta4_avg_ms": baseline_ms,
        "beta4_delta_ms": delta_ms,
        "beta4_delta_percent": delta_percent,
        "executable_changed": executable_changed,
        "risk_flags": risks,
        "insufficient_reasons": insufficient,
        "verdict": verdict,
        "noise_model": {
            "baseline_runs": len(means),
            "stdev_ms": stdev_ms,
            "sigma": noise_sigma,
            "noise_band_ms": noise_band_ms,
        },
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, action="append", default=[])
    parser.add_argument("--minimum-samples", type=int, default=30)
    parser.add_argument("--noise-sigma", type=float, default=2.0)
    parser.add_argument("--minimum-improvement-percent", type=float, default=2.0)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    candidate = analyze_file(args.candidate)
    baselines = [analyze_file(path) for path in args.baseline]
    report: dict[str, Any] = {"candidate": candidate, "baselines": baselines}
    if baselines:
        report["comparison"] = compare(
            baselines,
            candidate,
            minimum_samples=args.minimum_samples,
            noise_sigma=args.noise_sigma,
            minimum_improvement_percent=args.minimum_improvement_percent,
        )

    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
        return
    timing = candidate["timing"]
    print(
        f"candidate: beta4_samples={timing['beta4_samples']} "
        f"beta4_avg_ms={timing['beta4_avg_ms']} "
        f"backend={candidate['backend']['name']}"
    )
    if "comparison" in report:
        result = report["comparison"]
        print(
            f"comparison: delta_ms={result['beta4_delta_ms']} "
            f"delta_percent={result['beta4_delta_percent']} "
            f"verdict={result['verdict']} "
            f"risks={','.join(result['risk_flags']) or 'none'}"
        )


if __name__ == "__main__":
    main()
