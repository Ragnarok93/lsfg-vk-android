#!/usr/bin/env python3
"""Guard the restored Adaptive Flow Scale Android build composition."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROFILE = (ROOT / "scripts/apply-adreno-evidence-profile.py").read_text(encoding="utf-8")
BUILD = (ROOT / "scripts/build/android.sh").read_text(encoding="utf-8")

FORBIDDEN_PROFILE_IMPORTS = (
    "adreno_syncfd_handoff",
    "adreno_async_zero_history",
    "adreno_slot_aware_zero_history",
    "adreno_transport_release_overlap",
    "adreno_deferred_zero",
    "adreno_nonblocking_generated_pipeline",
)
for token in FORBIDDEN_PROFILE_IMPORTS:
    assert token not in PROFILE, f"restored evidence profile must not compose {token}"

FORBIDDEN_BUILD_INVOCATIONS = (
    "scripts/adreno_nonblocking_generated_pipeline.py",
    "scripts/adreno_deferred_zero",
    "scripts/adreno_async_zero_history",
)
for token in FORBIDDEN_BUILD_INVOCATIONS:
    assert token not in BUILD, f"restored Android build must not invoke {token}"

# Device-proven Adreno shader/hot-path optimizations stay in the build.
for retained in (
    "apply-candidate-b13-beta4-fused-mask.py",
    "apply-candidate-b14-mipmaps-tail-fusion.py",
    "apply-android-command-buffer-reuse.py",
    "apply-android-submit-hot-path.py",
):
    assert retained in BUILD, f"retained optimization disappeared: {retained}"
