#!/usr/bin/env python3
"""Add portable Vulkan device metadata and finalize B12 reporting hardening."""
from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys

from adreno_evidence_common import replace_exact

SOURCE = Path("framegen/src/core/timestampquerypool.cpp")
HARDENING = Path("scripts/apply-b12-reporting-hardening.py")


def apply(root: Path) -> None:
    path = root / SOURCE
    text = path.read_text(encoding="utf-8")
    if "b12-device-profile" not in text:
        anchor = "    static std::atomic<bool> capabilityLogged{false};\n"
        block = r'''    static std::atomic<bool> deviceProfileLogged{false};
    if (!deviceProfileLogged.exchange(true)) {
        std::cerr << "lsfg-vk: b12-device-profile vendor_id=0x" << std::hex
                  << properties.vendorID
                  << " device_id=0x" << properties.deviceID << std::dec
                  << " device_name=\"" << properties.deviceName << "\""
                  << " driver_version=" << properties.driverVersion
                  << " api_version=" << properties.apiVersion
                  << " timestamp_period_ns=" << this->timestampPeriodNs_
                  << " compute_family=" << familyIndex << '\n';
    }

    static std::atomic<bool> capabilityLogged{false};
'''
        text = replace_exact(
            text,
            anchor,
            block,
            count=1,
            label=f"{path}: B12 portable device profile",
        )
        path.write_text(text, encoding="utf-8")

    hardening = root / HARDENING
    if not hardening.is_file():
        raise RuntimeError(f"B12 reporting hardening transform missing: {hardening}")
    subprocess.run(
        [sys.executable, str(hardening), "--root", str(root)],
        check=True,
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    apply(args.root.resolve())


if __name__ == "__main__":
    main()
