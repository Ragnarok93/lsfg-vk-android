#!/usr/bin/env python3
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
build = (root / "scripts/build/android.sh").read_text(encoding="utf-8")
stale_switch = re.compile(
    r"LSFGVK_(?:ADAPTIVE_RUNTIME|B11|B12|ZERO_STAGE|B8_DIAGNOSTICS|"
    r"FINAL_NONADAPTIVE_SWEEP|EXPERIMENTAL_B9|MIPMAPS_EXEC_PROFILE|MIPMAPS_CANDIDATE_SCRIPT)"
)
match = stale_switch.search(build)
if match:
    raise SystemExit(f"stale development switch remains in production Android build: {match.group(0)}")

required = [
    "adreno_suspend_timeout_guard.py",
    "adreno_android_runtime_residency.py",
    "adreno_android_config_reload.py",
    "apply-adreno-evidence-profile.py",
    "apply-candidate-b-translation-cleanup.py",
    "apply-candidate-b4-beta4-predicate.py",
    "apply-candidate-b11-beta4-pow2-mask.py",
    "apply-candidate-b13-beta4-fused-mask.py",
    "apply-candidate-b14-mipmaps-tail-fusion.py",
    "apply-android-command-buffer-reuse.py",
    "apply-android-submit-hot-path.py",
]
positions = []
for token in required:
    if token not in build:
        raise SystemExit(f"production transform missing: {token}")
    positions.append(build.index(token))
if positions != sorted(positions):
    raise SystemExit("production Android transform ordering regressed")

composer = (root / "scripts/apply-adreno-evidence-profile.py").read_text(encoding="utf-8")
for stale in (
    "apply_profiling(",
    "adreno_evidence_outer",
    "adreno_evidence_framegen",
    "--runtime-only",
    "FRAMEGEN_HEADERS",
    "FRAMEGEN_SOURCES",
):
    if stale in composer:
        raise SystemExit(f"retained runtime composer still contains profiler code: {stale}")

removed = [
    "scripts/apply-b12-dual-stage-profile.py",
    "scripts/apply-zero-stage-profile.py",
    "scripts/apply-candidate-b3-beta4-analysis.py",
    "scripts/apply-candidate-b9-beta4-spill-collapse.py",
    "scripts/analyze-mipmaps-evidence.py",
    "tests/android_zero_stage_profile_test.py",
    "tests/android_candidate_b3_beta4_analysis_test.py",
    "tests/android_candidate_b9_beta4_spill_collapse_test.py",
    "tests/android_b12_timestamp_fallback_test.py",
    "include/adreno_source_protection.hpp",
    "tests/android_adreno_source_protection_state_test.cpp",
    "tests/android_adreno_source_protection_delivery_test.py",
]
for rel in removed:
    if (root / rel).exists():
        raise SystemExit(f"superseded experiment artifact remains: {rel}")

source_protection_tokens = (
    "AdrenoSourceProtectionController",
    "AdrenoSourceProtectionBackoffReason",
    "adrenoSourceProtection_",
    "SourceProtectionBudgetTracker",
    "SourceProtectionBudgetTelemetry",
    "sourceProtectionBudgetTracker_",
    "setSourceProtectionBaseline(",
    "source_protection_baseline_valid=",
    "source_protection_copy_cost_valid=",
    "adreno_source_protection_state=",
    "adreno_source_protection_backoff=",
)
for rel in (
    "include/adaptive_scheduler.hpp",
    "src/adaptive_scheduler.cpp",
    "include/context.hpp",
    "src/context.cpp",
):
    text = (root / rel).read_text(encoding="utf-8")
    for token in source_protection_tokens:
        if token in text:
            raise SystemExit(f"retired source-protection mechanism remains in {rel}: {token}")

print("production Android pipeline cleanup contract: ok")
