"""Device-agnostic Mipmaps evidence parsing and comparison."""
from __future__ import annotations

import hashlib
import re
import statistics
from pathlib import Path
from typing import Any, Iterable

from mipmaps_evidence.backends import ir3

TIMING_RE = re.compile(
    r"b12-stage-profile\s+"
    r"mipmaps_samples=(?P<mip_samples>\d+)\s+"
    r"mipmaps_avg_ms=(?P<mip_ms>[0-9.eE+-]+)\s+"
    r"beta4_samples=(?P<beta_samples>\d+)\s+"
    r"beta4_avg_ms=(?P<beta_ms>[0-9.eE+-]+)"
)
KV_RE = re.compile(r'(?P<key>[A-Za-z0-9_]+)=(?:"(?P<quoted>(?:\\.|[^"])*)"|(?P<bare>\S+))')
PROPERTY_RE = re.compile(
    r'pipeline-exec-property shader=p_mipmaps executable=(?P<exec>\d+).*?'
    r'subgroup_size=(?P<subgroup>\d+).*?name="(?P<name>(?:\\.|[^"])*)"'
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


def _unescape(text: str) -> str:
    output: list[str] = []
    i = 0
    while i < len(text):
        if text[i] != "\\" or i + 1 >= len(text):
            output.append(text[i])
            i += 1
            continue
        nxt = text[i + 1]
        output.append({"n": "\n", "r": "\r", "t": "\t"}.get(nxt, nxt))
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


def _stat_name(name: str) -> str:
    return re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_") or "unnamed"


def _parse_device_profile(raw_line: str) -> dict[str, Any]:
    marker = "b12-device-profile "
    if marker not in raw_line:
        return {}
    payload = raw_line.split(marker, 1)[1]
    values: dict[str, Any] = {}
    for match in KV_RE.finditer(payload):
        raw = match.group("quoted") if match.group("quoted") is not None else match.group("bare")
        value = _unescape(raw)
        if match.group("key") == "compute_family":
            value = int(value, 0)
        elif match.group("key") == "timestamp_period_ns":
            value = float(value)
        values[match.group("key")] = value
    return values


def analyze_text(text: str, backend: str = "auto") -> dict[str, Any]:
    mip_samples = 0
    mip_weighted_ms = 0.0
    beta_samples = 0
    beta_weighted_ms = 0.0
    device: dict[str, Any] = {}
    executable_names: list[str] = []
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

        parsed_device = _parse_device_profile(raw_line)
        if parsed_device:
            device = parsed_device

        prop = PROPERTY_RE.search(raw_line)
        if prop:
            executable_names.append(_unescape(prop.group("name")))
            subgroup_sizes.append(int(prop.group("subgroup")))

        stat = STAT_RE.search(raw_line)
        if stat:
            stats[_stat_name(_unescape(stat.group("name")))] = _number(stat.group("value"))

        ir_match = IR_RE.search(raw_line)
        if ir_match:
            key = (int(ir_match.group("exec")), int(ir_match.group("ir")), int(ir_match.group("line")))
            chunks.setdefault(key, {})[int(ir_match.group("chunk"))] = _unescape(ir_match.group("text"))

    lines = ["".join(chunks[key][idx] for idx in sorted(chunks[key])) for key in sorted(chunks)]
    ir_text = "\n".join(lines)
    executable = {
        "present": bool(lines or executable_names),
        "sha256": hashlib.sha256(ir_text.encode("utf-8")).hexdigest() if lines else None,
        "line_count": len(lines),
        "names": sorted(set(executable_names)),
        "subgroup_sizes": sorted(set(subgroup_sizes)),
        "statistics": stats,
    }

    selected = "unsupported" if executable["present"] else "none"
    backend_metrics: dict[str, Any] = {}
    if backend == "ir3" or (backend == "auto" and ir3.supports(executable_names, lines)):
        selected = "ir3"
        backend_metrics = ir3.analyze(lines, stats)
    elif backend not in ("auto", "none"):
        raise ValueError(f"unsupported backend adapter: {backend}")

    return {
        "device": device,
        "timing": {
            "mipmaps_samples": mip_samples,
            "mipmaps_avg_ms": (mip_weighted_ms / mip_samples) if mip_samples else None,
            "beta4_samples": beta_samples,
            "beta4_avg_ms": (beta_weighted_ms / beta_samples) if beta_samples else None,
        },
        "executable": executable,
        "backend": {"name": selected, "available": selected == "ir3", "metrics": backend_metrics},
    }


def analyze_file(path: Path, backend: str = "auto") -> dict[str, Any]:
    return analyze_text(path.read_text(encoding="utf-8", errors="replace"), backend=backend)


def _weighted_baseline_ms(baselines: Iterable[dict[str, Any]]) -> float | None:
    total_samples = 0
    weighted = 0.0
    for item in baselines:
        samples = item["timing"]["mipmaps_samples"]
        value = item["timing"]["mipmaps_avg_ms"]
        if samples and value is not None:
            total_samples += samples
            weighted += samples * value
    return weighted / total_samples if total_samples else None


def _device_key(item: dict[str, Any]) -> tuple[Any, ...] | None:
    device = item.get("device") or {}
    required = ("vendor_id", "device_id", "device_name")
    if not all(key in device for key in required):
        return None
    optional = tuple(
        (key, device[key])
        for key in ("driver_version", "driver_name", "driver_info", "api_version")
        if key in device
    )
    return tuple(device[key] for key in required) + optional


def compare(
    baselines: list[dict[str, Any]],
    candidate: dict[str, Any],
    *,
    minimum_samples: int = 30,
    noise_sigma: float = 2.0,
    minimum_improvement_percent: float = 2.0,
) -> dict[str, Any]:
    baseline_ms = _weighted_baseline_ms(baselines)
    candidate_ms = candidate["timing"]["mipmaps_avg_ms"]
    run_means = [item["timing"]["mipmaps_avg_ms"] for item in baselines if item["timing"]["mipmaps_avg_ms"] is not None]
    stdev_ms = statistics.stdev(run_means) if len(run_means) >= 2 else None
    noise_band_ms = noise_sigma * stdev_ms if stdev_ms is not None else None

    delta_ms = None
    delta_percent = None
    if baseline_ms is not None and candidate_ms is not None:
        delta_ms = candidate_ms - baseline_ms
        if baseline_ms:
            delta_percent = (delta_ms / baseline_ms) * 100.0

    reasons: list[str] = []
    if len(run_means) < 2:
        reasons.append("baseline_noise_model")
    if candidate["timing"]["mipmaps_samples"] < minimum_samples:
        reasons.append("candidate_samples")
    if delta_ms is None:
        reasons.append("timing")

    candidate_key = _device_key(candidate)
    baseline_keys = {_device_key(item) for item in baselines if _device_key(item) is not None}
    if candidate_key is not None and baseline_keys and (len(baseline_keys) != 1 or candidate_key not in baseline_keys):
        reasons.append("device_mismatch")

    baseline_hashes = {item["executable"]["sha256"] for item in baselines if item["executable"]["sha256"]}
    candidate_hash = candidate["executable"]["sha256"]
    executable_changed = None
    if len(baseline_hashes) == 1 and candidate_hash:
        executable_changed = candidate_hash not in baseline_hashes

    risk_flags: list[str] = []
    if baselines and candidate["backend"]["available"]:
        reference = baselines[0]
        if reference["backend"]["name"] == candidate["backend"]["name"] == "ir3":
            risk_flags.extend(ir3.compare(reference["backend"]["metrics"], candidate["backend"]["metrics"]))

    if risk_flags:
        verdict = "reject"
    elif reasons:
        verdict = "insufficient"
    elif delta_ms is None or delta_percent is None or noise_band_ms is None:
        verdict = "insufficient"
    elif delta_ms < -noise_band_ms and delta_percent <= -minimum_improvement_percent and executable_changed is not False:
        verdict = "promising"
    elif delta_ms > noise_band_ms and delta_percent >= minimum_improvement_percent:
        verdict = "regression"
    elif abs(delta_ms) <= noise_band_ms or abs(delta_percent) < minimum_improvement_percent:
        verdict = "noise"
    else:
        verdict = "inconclusive"

    return {
        "baseline_mipmaps_avg_ms": baseline_ms,
        "mipmaps_delta_ms": delta_ms,
        "mipmaps_delta_percent": delta_percent,
        "executable_changed": executable_changed,
        "risk_flags": risk_flags,
        "insufficient_reasons": reasons,
        "verdict": verdict,
        "noise_model": {"baseline_runs": len(run_means), "stdev_ms": stdev_ms, "sigma": noise_sigma, "noise_band_ms": noise_band_ms},
        "thresholds": {"minimum_samples": minimum_samples, "minimum_improvement_percent": minimum_improvement_percent},
    }


def legacy_compare(baseline: dict[str, Any], candidate: dict[str, Any]) -> dict[str, Any]:
    base_ms = baseline["timing"]["mipmaps_avg_ms"]
    cand_ms = candidate["timing"]["mipmaps_avg_ms"]
    delta_ms = cand_ms - base_ms if base_ms is not None and cand_ms is not None else None
    delta_percent = (delta_ms / base_ms) * 100.0 if delta_ms is not None and base_ms else None
    risk_flags: list[str] = []
    if baseline["backend"]["name"] == candidate["backend"]["name"] == "ir3":
        risk_flags = ir3.compare(baseline["backend"]["metrics"], candidate["backend"]["metrics"])
    hashes = (baseline["executable"]["sha256"], candidate["executable"]["sha256"])
    changed = bool(hashes[0] and hashes[1] and hashes[0] != hashes[1])
    samples = candidate["timing"]["mipmaps_samples"]
    if risk_flags:
        verdict = "reject"
    elif samples < 30 or delta_ms is None:
        verdict = "insufficient"
    elif delta_ms <= -0.10 and changed:
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
        "executable_changed": changed,
        "risk_flags": risk_flags,
        "verdict": verdict,
        "thresholds": {"minimum_samples": 30, "noise_band_ms": 0.05, "material_improvement_ms": 0.10},
    }
