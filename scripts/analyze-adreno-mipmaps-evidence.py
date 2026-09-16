#!/usr/bin/env python3
"""Summarize and compare Adreno Mipmaps timing + pipeline executable evidence.

The analyzer intentionally treats final executable/IR evidence and measured GPU
time as the acceptance signals.  SPIR-V byte count alone is not considered an
optimization result.  It can consume ordinary B12 logs, optional B6 Mipmaps
pipeline-executable logs, or both.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path
from typing import Any

TIMING_RE = re.compile(
    r"b12-stage-profile\s+"
    r"mipmaps_samples=(?P<mip_samples>\d+)\s+"
    r"mipmaps_avg_ms=(?P<mip_ms>[0-9.eE+-]+)\s+"
    r"beta4_samples=(?P<beta_samples>\d+)\s+"
    r"beta4_avg_ms=(?P<beta_ms>[0-9.eE+-]+)"
)
PROPERTY_RE = re.compile(
    r"pipeline-exec-property shader=p_mipmaps executable=(?P<exec>\d+).*?"
    r"subgroup_size=(?P<subgroup>\d+)"
)
STAT_RE = re.compile(
    r"pipeline-exec-stat shader=p_mipmaps executable=(?P<exec>\d+) "
    r"stat=(?P<stat>\d+) format=(?P<format>\S+) value=(?P<value>\S+) "
    r'name="(?P<name>(?:\\.|[^"])*)"'
)
IR_RE = re.compile(
    r"pipeline-exec-ir-line shader=p_mipmaps executable=(?P<exec>\d+) "
    r"ir=(?P<ir>\d+) line=(?P<line>\d+) chunk=(?P<chunk>\d+) "
    r"chunks=(?P<chunks>\d+) text=\"(?P<text>.*)\"$"
)
REGISTER_RE = re.compile(r"\br(?P<index>\d+)(?:\.[xyzw]+)?\b", re.IGNORECASE)


def _unescape(text: str) -> str:
    output: list[str] = []
    i = 0
    while i < len(text):
        if text[i] != "\\" or i + 1 >= len(text):
            output.append(text[i])
            i += 1
            continue
        nxt = text[i + 1]
        if nxt == "n":
            output.append("\n")
        elif nxt == "r":
            output.append("\r")
        elif nxt == "t":
            output.append("\t")
        else:
            output.append(nxt)
        i += 2
    return "".join(output)


def _number(value: str) -> int | float | str:
    try:
        return int(value, 0)
    except ValueError:
        try:
            return float(value)
        except ValueError:
            return value


def _normalized_stat_name(name: str) -> str:
    return re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_") or "unnamed"


def analyze_text(text: str) -> dict[str, Any]:
    mip_samples = 0
    mip_weighted_ms = 0.0
    beta_samples = 0
    beta_weighted_ms = 0.0
    subgroup_sizes: list[int] = []
    stats: dict[str, int | float | str] = {}
    chunks: dict[tuple[int, int, int], dict[int, str]] = {}

    for raw_line in text.splitlines():
        timing = TIMING_RE.search(raw_line)
        if timing:
            m_samples = int(timing.group("mip_samples"))
            b_samples = int(timing.group("beta_samples"))
            mip_samples += m_samples
            beta_samples += b_samples
            mip_weighted_ms += m_samples * float(timing.group("mip_ms"))
            beta_weighted_ms += b_samples * float(timing.group("beta_ms"))

        prop = PROPERTY_RE.search(raw_line)
        if prop:
            subgroup_sizes.append(int(prop.group("subgroup")))

        stat = STAT_RE.search(raw_line)
        if stat:
            name = _normalized_stat_name(_unescape(stat.group("name")))
            stats[name] = _number(stat.group("value"))

        ir = IR_RE.search(raw_line)
        if ir:
            key = (int(ir.group("exec")), int(ir.group("ir")), int(ir.group("line")))
            chunks.setdefault(key, {})[int(ir.group("chunk"))] = _unescape(ir.group("text"))

    reconstructed: list[str] = []
    for key in sorted(chunks):
        reconstructed.append("".join(chunks[key][idx] for idx in sorted(chunks[key])))
    ir_text = "\n".join(reconstructed)
    instruction_lines = [
        line for line in reconstructed
        if line.strip() and not line.lstrip().startswith((";", "#", "//"))
    ]

    lowered_lines = [line.lower() for line in instruction_lines]
    register_indices = [
        int(match.group("index"))
        for line in instruction_lines
        for match in REGISTER_RE.finditer(line)
    ]
    spill_pattern = re.compile(r"\b(?:spill|scratch|reload)\b", re.IGNORECASE)

    resident_waves = None
    for key, value in stats.items():
        if "wave" in key and isinstance(value, (int, float)):
            resident_waves = value
            break

    return {
        "timing": {
            "mipmaps_samples": mip_samples,
            "mipmaps_avg_ms": (mip_weighted_ms / mip_samples) if mip_samples else None,
            "beta4_samples": beta_samples,
            "beta4_avg_ms": (beta_weighted_ms / beta_samples) if beta_samples else None,
        },
        "pipeline": {
            "subgroup_sizes": sorted(set(subgroup_sizes)),
            "statistics": stats,
            "resident_waves": resident_waves,
        },
        "ir": {
            "present": bool(reconstructed),
            "sha256": hashlib.sha256(ir_text.encode("utf-8")).hexdigest() if reconstructed else None,
            "instruction_lines": len(instruction_lines),
            "nop_count": sum(bool(re.search(r"\bnop\b", line)) for line in lowered_lines),
            "shared_load_count": sum(bool(re.search(r"\bldl\b", line)) for line in lowered_lines),
            "shared_store_count": sum(bool(re.search(r"\bstl\b", line)) for line in lowered_lines),
            "global_load_count": sum(bool(re.search(r"\bldg\b", line)) for line in lowered_lines),
            "global_store_count": sum(bool(re.search(r"\bstg\b", line)) for line in lowered_lines),
            "ss_count": sum(bool(re.search(r"(?:\(ss\)|\bss\b)", line)) for line in lowered_lines),
            "sy_count": sum(bool(re.search(r"(?:\(sy\)|\bsy\b)", line)) for line in lowered_lines),
            "spill_markers": sum(len(spill_pattern.findall(line)) for line in instruction_lines),
            "max_register_index": max(register_indices) if register_indices else None,
        },
    }


def analyze_file(path: Path) -> dict[str, Any]:
    return analyze_text(path.read_text(encoding="utf-8", errors="replace"))


def compare(baseline: dict[str, Any], candidate: dict[str, Any]) -> dict[str, Any]:
    base_timing = baseline["timing"]
    cand_timing = candidate["timing"]
    base_ir = baseline["ir"]
    cand_ir = candidate["ir"]
    base_pipeline = baseline["pipeline"]
    cand_pipeline = candidate["pipeline"]

    delta_ms = None
    delta_percent = None
    if base_timing["mipmaps_avg_ms"] is not None and cand_timing["mipmaps_avg_ms"] is not None:
        delta_ms = cand_timing["mipmaps_avg_ms"] - base_timing["mipmaps_avg_ms"]
        if base_timing["mipmaps_avg_ms"] != 0:
            delta_percent = (delta_ms / base_timing["mipmaps_avg_ms"]) * 100.0

    risk_flags: list[str] = []
    if cand_ir["spill_markers"] > base_ir["spill_markers"]:
        risk_flags.append("spill_markers_increased")
    base_waves = base_pipeline["resident_waves"]
    cand_waves = cand_pipeline["resident_waves"]
    if isinstance(base_waves, (int, float)) and isinstance(cand_waves, (int, float)):
        if cand_waves < base_waves:
            risk_flags.append("resident_waves_decreased")

    ir_changed = bool(base_ir["sha256"] and cand_ir["sha256"] and base_ir["sha256"] != cand_ir["sha256"])
    candidate_samples = cand_timing["mipmaps_samples"]

    if risk_flags:
        verdict = "reject"
    elif candidate_samples < 30 or delta_ms is None:
        verdict = "insufficient"
    elif delta_ms <= -0.10 and ir_changed:
        verdict = "promising"
    elif abs(delta_ms) < 0.05:
        verdict = "noise"
    elif delta_ms >= 0.10:
        verdict = "regression"
    else:
        verdict = "inconclusive"

    return {
        "mipmaps_delta_ms": delta_ms,
        "mipmaps_delta_percent": delta_percent,
        "ir_changed": ir_changed,
        "risk_flags": risk_flags,
        "verdict": verdict,
        "thresholds": {
            "minimum_samples": 30,
            "noise_band_ms": 0.05,
            "material_improvement_ms": 0.10,
        },
    }


def _print_text(report: dict[str, Any]) -> None:
    candidate = report["candidate"]
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
        comparison = report["comparison"]
        print(
            f"comparison: delta_ms={comparison['mipmaps_delta_ms']} "
            f"delta_percent={comparison['mipmaps_delta_percent']} "
            f"verdict={comparison['verdict']} risks={','.join(comparison['risk_flags']) or 'none'}"
        )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    report: dict[str, Any] = {"candidate": analyze_file(args.candidate)}
    if args.baseline is not None:
        report["baseline"] = analyze_file(args.baseline)
        report["comparison"] = compare(report["baseline"], report["candidate"])

    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        _print_text(report)


if __name__ == "__main__":
    main()
