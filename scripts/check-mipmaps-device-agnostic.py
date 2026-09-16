#!/usr/bin/env python3
"""Reject device-specific identity branching from a Mipmaps refinement transform."""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

FORBIDDEN = (
    r"\bvendorID\b",
    r"\bdeviceID\b",
    r"\bdeviceName\b",
    r"\bdriverName\b",
    r"\bdriverInfo\b",
    r"\bvendor_id\b",
    r"\bdevice_id\b",
    r"\bAdreno(?:\s*\d+)?\b",
    r"\bMali(?:[-_A-Za-z0-9]*)?\b",
    r"\bXclipse(?:\s*\d+)?\b",
    r"\bTurnip\b",
    r"\bQualcomm\b",
    r"\bNVIDIA\b",
    r"\bRADV\b",
)


def violations(text: str) -> list[str]:
    found: list[str] = []
    for pattern in FORBIDDEN:
        match = re.search(pattern, text, re.IGNORECASE if pattern[2:].startswith(("Adreno", "Mali", "Xclipse", "Turnip", "Qualcomm", "NVIDIA", "RADV")) else 0)
        if match:
            found.append(match.group(0))
    return found


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("candidate", type=Path)
    args = parser.parse_args()
    text = args.candidate.read_text(encoding="utf-8", errors="replace")
    found = violations(text)
    if found:
        print(
            "error: Mipmaps refinement transforms must be device agnostic; "
            "identity-specific token(s): " + ", ".join(sorted(set(found))),
            file=sys.stderr,
        )
        raise SystemExit(2)
    print(f"device-agnostic Mipmaps candidate check passed: {args.candidate}")


if __name__ == "__main__":
    main()
