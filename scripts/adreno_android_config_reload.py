#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path

from adreno_evidence_common import replace_exact


def apply(root: Path) -> None:
    path = root / "src/context.cpp"
    text = path.read_text(encoding="utf-8")
    if "android-config-reload-no-sleep" in text:
        return

    old = """        std::cerr << \"lsfg-vk: Rereading configuration, as it is no longer valid.\\n\";\n        std::this_thread::sleep_for(std::chrono::milliseconds(100));\n\n        // reread configuration\n"""
    new = """        std::cerr << \"lsfg-vk: Rereading configuration, as it is no longer valid.\\n\";\n        // android-config-reload-no-sleep: GameNative publishes conf.toml by\n        // atomic rename, so Android does not need the legacy write-settle delay.\n#ifndef __ANDROID__\n        std::this_thread::sleep_for(std::chrono::milliseconds(100));\n#endif\n\n        // reread configuration\n"""
    text = replace_exact(
        text,
        old,
        new,
        count=1,
        label=f"{path}: remove Android config reload stall",
    )
    path.write_text(text, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    apply(args.root.resolve())


if __name__ == "__main__":
    main()
