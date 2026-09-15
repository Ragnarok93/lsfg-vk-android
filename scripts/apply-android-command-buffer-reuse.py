#!/usr/bin/env python3
"""Reuse per-slot framegen command buffers in Android builds.

Runs after all optional profiling/source transforms so it cannot disturb their
anchors. The existing slot fence/preprocessing retirement guarantees completion
before a Submitted command buffer is reset and re-recorded.
"""
from __future__ import annotations

import argparse
from pathlib import Path

SOURCES = (
    Path("framegen/v3.1_src/context.cpp"),
    Path("framegen/v3.1p_src/context.cpp"),
)
MARKER = "android-command-buffer-reuse"


def replace_exact(text: str, old: str, new: str, count: int, label: str) -> str:
    found = text.count(old)
    if found != count:
        raise RuntimeError(f"{label}: expected {count} anchor(s), found {found}")
    return text.replace(old, new, count)


def patch(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if MARKER in text:
        return

    text = replace_exact(
        text,
        "        data.cmdBuffers2.resize(vk.generationCount);\n",
        "        data.cmdBuffer1 = Core::CommandBuffer(vk.device, vk.commandPool);\n"
        "        data.cmdBuffers2.resize(vk.generationCount);\n"
        "        for (auto& cmdBuffer : data.cmdBuffers2)\n"
        "            cmdBuffer = Core::CommandBuffer(vk.device, vk.commandPool);\n",
        2,
        f"{path}: slot command-buffer initialization",
    )

    text = replace_exact(
        text,
        "    data.cmdBuffer1 = Core::CommandBuffer(vk.device, vk.commandPool);\n"
        "    data.cmdBuffer1.begin();\n",
        "    // android-command-buffer-reuse: slot retirement above guarantees completion.\n"
        "    if (data.cmdBuffer1.getState() == Core::CommandBufferState::Submitted)\n"
        "        data.cmdBuffer1.reset();\n"
        "    data.cmdBuffer1.begin();\n",
        1,
        f"{path}: primary command-buffer reuse",
    )

    text = replace_exact(
        text,
        "        auto& buf2 = data.cmdBuffers2.at(pass);\n"
        "        buf2 = Core::CommandBuffer(vk.device, vk.commandPool);\n"
        "        buf2.begin();\n",
        "        auto& buf2 = data.cmdBuffers2.at(pass);\n"
        "        if (buf2.getState() == Core::CommandBufferState::Submitted)\n"
        "            buf2.reset();\n"
        "        buf2.begin();\n",
        1,
        f"{path}: generated-pass command-buffer reuse",
    )

    path.write_text(text, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    root = args.root.resolve()
    for relative in SOURCES:
        patch(root / relative)


if __name__ == "__main__":
    main()
