"""Optional Turnip/IR3 executable-evidence adapter for Mipmaps analysis."""
from __future__ import annotations

import re
from typing import Any

REGISTER_RE = re.compile(r"\br(?P<index>\d+)(?:\.[xyzw]+)?\b", re.IGNORECASE)
SPILL_RE = re.compile(r"\b(?:spill|scratch|reload)\b", re.IGNORECASE)


def supports(names: list[str], lines: list[str]) -> bool:
    return any("ir3" in name.lower() for name in names) or any(
        re.search(r"\b(?:ldl|stl)\b", line, re.IGNORECASE) for line in lines
    )


def analyze(lines: list[str], statistics: dict[str, int | float | str]) -> dict[str, Any]:
    instruction_lines = [
        line for line in lines
        if line.strip() and not line.lstrip().startswith((";", "#", "//"))
    ]
    lowered = [line.lower() for line in instruction_lines]
    registers = [
        int(match.group("index"))
        for line in instruction_lines
        for match in REGISTER_RE.finditer(line)
    ]
    resident_waves = None
    for key, value in statistics.items():
        if "wave" in key and isinstance(value, (int, float)):
            resident_waves = value
            break
    return {
        "instruction_lines": len(instruction_lines),
        "nop_count": sum(bool(re.search(r"\bnop\b", line)) for line in lowered),
        "shared_load_count": sum(bool(re.search(r"\bldl\b", line)) for line in lowered),
        "shared_store_count": sum(bool(re.search(r"\bstl\b", line)) for line in lowered),
        "global_load_count": sum(bool(re.search(r"\bldg\b", line)) for line in lowered),
        "global_store_count": sum(bool(re.search(r"\bstg\b", line)) for line in lowered),
        "ss_count": sum(bool(re.search(r"(?:\(ss\)|\bss\b)", line)) for line in lowered),
        "sy_count": sum(bool(re.search(r"(?:\(sy\)|\bsy\b)", line)) for line in lowered),
        "spill_markers": sum(len(SPILL_RE.findall(line)) for line in instruction_lines),
        "max_register_index": max(registers) if registers else None,
        "resident_waves": resident_waves,
    }


def compare(baseline: dict[str, Any], candidate: dict[str, Any]) -> list[str]:
    risks: list[str] = []
    if candidate.get("spill_markers", 0) > baseline.get("spill_markers", 0):
        risks.append("spill_markers_increased")
    base_waves = baseline.get("resident_waves")
    cand_waves = candidate.get("resident_waves")
    if isinstance(base_waves, (int, float)) and isinstance(cand_waves, (int, float)) and cand_waves < base_waves:
        risks.append("resident_waves_decreased")
    return risks
