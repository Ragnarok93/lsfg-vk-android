#!/usr/bin/env python3
"""Add portable Vulkan device metadata to B12 evidence logs."""
from __future__ import annotations

import argparse
from pathlib import Path

from adreno_evidence_common import replace_exact

SOURCE = Path("framegen/src/core/timestampquerypool.cpp")


def apply(root: Path) -> None:
    path = root / SOURCE
    text = path.read_text(encoding="utf-8")
    if "b12-device-profile" in text:
        return
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


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    apply(args.root.resolve())


if __name__ == "__main__":
    main()
