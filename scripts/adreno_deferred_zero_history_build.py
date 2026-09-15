#!/usr/bin/env python3
"""Build-pipeline wrapper for Candidate A's composed-source epoch insertion.

The Candidate A transform intentionally inserts its DeferredZero fast path before
the pre-existing maintained-zero branch.  That creates two telemetry increment
landmarks in the composed source.  For the one epoch-advance edit, the normal
maintained-zero branch is the later landmark, so use a rightmost single replace
for that label while preserving strict exact-match behavior everywhere else.
"""
from __future__ import annotations

from pathlib import Path

import adreno_deferred_zero_history as candidate_a


_ORIGINAL_ONCE = candidate_a.once
_EPOCH_LABEL = "advance framegen history epoch on maintained zero"


def _composed_once(text: str, old: str, new: str, label: str) -> str:
    if _EPOCH_LABEL not in label:
        return _ORIGINAL_ONCE(text, old, new, label)
    found = text.count(old)
    if found < 1:
        raise RuntimeError(f"{label}: expected at least 1 match, found 0")
    index = text.rfind(old)
    return text[:index] + new + text[index + len(old):]


def apply(root: Path) -> None:
    previous = candidate_a.once
    candidate_a.once = _composed_once
    try:
        candidate_a.apply(root)
    finally:
        candidate_a.once = previous
